/* port/3ds/net/gdx3ds_mp_ipcstat.c — NETTICK: per-kind latency of the network sysmodule calls.
 * Contract: gdx3ds_mp_ipcstat.h. */
#include "gdx3ds_mp_ipcstat.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define IPC_HIST 4

typedef struct IpcKind {
    uint32_t calls;
    uint32_t gated;
    uint64_t ticks;
    uint64_t maxTicks;
    uint32_t hist[IPC_HIST]; /* <250 us, <1 ms, <4 ms, >= 4 ms */
} IpcKind;

static IpcKind sKind[GDX_IPC_KINDS];
static const char* const kKindName[GDX_IPC_KINDS] = { "uds_pull", "uds_send", "soc_recv", "soc_send", "disc_recv",
                                                       "q_push", "q_pop", "st_recv", "st_send", "st_poll",
                                                       "disc_send" };
static LightLock sLock;
static volatile int sLockInit = 0;

static void Lock(void) {
    if (!sLockInit) {
        LightLock_Init(&sLock); /* first use is on the main thread, before the service thread starts */
        sLockInit = 1;
    }
    LightLock_Lock(&sLock);
}

static void Unlock(void) {
    LightLock_Unlock(&sLock);
}

void gdx3ds_ipcstat_init(void) {
    Lock();
    Unlock();
}

uint64_t gdx3ds_ipcstat_begin(void) {
    return svcGetSystemTick();
}

static int Bucket(uint64_t ticks) {
    const uint64_t us = ticks / (uint64_t) CPU_TICKS_PER_USEC;
    if (us < 250u) {
        return 0;
    } else if (us < 1000u) {
        return 1;
    } else if (us < 4000u) {
        return 2;
    } else {
        return 3;
    }
}

void gdx3ds_ipcstat_end(int kind, uint64_t t0) {
    const uint64_t dt = svcGetSystemTick() - t0;
    IpcKind* k;
    if (kind < 0 || kind >= GDX_IPC_KINDS) {
        return;
    }
    Lock();
    k = &sKind[kind];
    k->calls++;
    k->ticks += dt;
    if (dt > k->maxTicks) {
        k->maxTicks = dt;
    }
    k->hist[Bucket(dt)]++;
    Unlock();
}

void gdx3ds_ipcstat_gated(int kind) {
    if (kind >= 0 && kind < GDX_IPC_KINDS) {
        Lock();
        sKind[kind].gated++;
        Unlock();
    }
}

void gdx3ds_ipcstat_line(char* buf, int cap, unsigned long frames) {
    int off;
    int i;
    int any = 0;
    if (cap <= 0) {
        return;
    }
    buf[0] = '\0';
    Lock();
    off = snprintf(buf, (size_t) cap, "[mpipc] frames=%lu", frames);
    for (i = 0; i < GDX_IPC_KINDS && off > 0 && off < cap; i++) {
        const IpcKind* k = &sKind[i];
        const unsigned long avgUs =
            k->calls ? (unsigned long) (k->ticks / (uint64_t) CPU_TICKS_PER_USEC / k->calls) : 0ul;
        if (k->calls == 0 && k->gated == 0) {
            continue;
        }
        any = 1;
        off += snprintf(buf + off, (size_t) (cap - off), " %s=%lu/%lu/%lu/h%lu,%lu,%lu,%lu gated=%lu", kKindName[i],
                        (unsigned long) k->calls, avgUs,
                        (unsigned long) (k->maxTicks / (uint64_t) CPU_TICKS_PER_USEC), (unsigned long) k->hist[0],
                        (unsigned long) k->hist[1], (unsigned long) k->hist[2], (unsigned long) k->hist[3],
                        (unsigned long) k->gated);
    }
    if (!any) {
        buf[0] = '\0';
    }
    memset(sKind, 0, sizeof(sKind));
    Unlock();
}
