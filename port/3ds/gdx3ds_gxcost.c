/* port/3ds/gdx3ds_gxcost.c -- onesubmit: [debug] gxcost, the CPU cost of GX / GSP dispatch per
 * thread (contract and receipt in include/gdx3ds_gxcost.h). The __wrap_* functions below replace
 * the named entry points for every caller outside their own libctru object (the link's --wrap
 * list in port/3ds/CMakeLists.txt). Zero heap. */
#include "gdx3ds_gxcost.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"

extern int gdx3ds_gputl_on(void) __attribute__((weak));
/* onesubmit (gfx/gdx3ds_stereo_frame.cpp): the [stereo] onesubmit A/B arm in force, -1 = none. */
extern int gdx3ds_stereo_onesub_arm(void) __attribute__((weak));
/* citro3d's context object; its first member is the frame's gxCmdQueue_s (see gdx3ds_overlap.c). */
extern char __C3D_Context[];

Result __real_GSPGPU_FlushDataCache(const void* adr, u32 size);
Result __real_svcFlushProcessDataCache(Handle process, u32 addr, u32 size);
Result __real_gspSubmitGxCommand(const u32 gxCommand[0x8]);
Result __real_GX_ProcessCommandList(u32* buf0a, u32 buf0s, u8 flags);
void __real_gxCmdQueueRun(gxCmdQueue_s* queue);
bool __real_gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout);
void __real_gspWaitForEvent(GSPGPU_Event id, bool nextEvent);
Result __real_svcWaitSynchronization(Handle handle, s64 nanoseconds);
bool __real_C3D_FrameBegin(u8 flags);
void __real_C3D_FrameEnd(u8 flags);

enum {
    GXC_FB = 0, /* C3D_FrameBegin */
    GXC_FE,     /* C3D_FrameEnd */
    GXC_QW,     /* gxCmdQueueWait */
    GXC_QR,     /* gxCmdQueueRun */
    GXC_PCL,    /* GX_ProcessCommandList */
    GXC_SUB,    /* gspSubmitGxCommand */
    GXC_GFL,    /* GSPGPU_FlushDataCache */
    GXC_SFL,    /* svcFlushProcessDataCache */
    GXC_GW,     /* gspWaitForEvent */
    GXC_COUNT
};

enum { THR_MAIN = 0, THR_EV, THR_OTH, THR_COUNT };
enum { IRQ_SLOTS = 7 }; /* GSPGPU_EVENT_PSC0 .. GSPGPU_EVENT_DMA */

/* Monotonic per-thread counters: each thread class only ever adds to its own row (the event thread
 * and main never write the same word); the receipt differences two snapshots, wrap-safe in u32
 * (the tick wraps a u32 every 16 s, a window is ~1 s). "oth" may race between its threads: it is
 * the least interesting row and a lost add there is acceptable for a diagnostic. */
typedef struct {
    volatile u32 calls[GXC_COUNT];
    volatile u32 ticks[GXC_COUNT];
    volatile u32 bytes[GXC_COUNT];
} ThreadRow;

typedef struct {
    ThreadRow row[THR_COUNT];
    volatile u32 evWakes;
    volatile u32 evBusy;
    volatile u32 irq[IRQ_SLOTS];
    u32 frames;
    u32 entries;
} Counters;

static int sLatched = 0;
static int sOn = 0;
static void* volatile sMainTls = NULL; /* the C3D frame's owner: the thread of the latest C3D_FrameBegin */
static void* volatile sEvTls = NULL;
static u32 sEvWakeTick = 0;
static int sEvAwake = 0;
static Counters sNow;
static Counters sPrev;
static u64 sWinStart = 0;
static int sWinArm = -1;

static void GxcLog(const char* fmt, ...) {
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    const size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    gdx3ds_filelog_write(buf, len);
}

static int ThreadClass(void) {
    void* tls = getThreadLocalStorage();
    if (tls == sMainTls) {
        return THR_MAIN;
    } else if (tls == sEvTls) {
        return THR_EV;
    }
    return THR_OTH;
}

static u32 Begin(void) {
    return sOn ? (u32)svcGetSystemTick() : 0u;
}

/* The first calls of each other thread are named once (thread id + site) so "oth" can be read. */
enum { OTH_NAMED_MAX = 4 };
static void* sOthNamed[OTH_NAMED_MAX];
static int sOthNamedN = 0;
static const char* const kSiteName[GXC_COUNT] = { "fb", "fe", "qw", "qr", "pcl", "sub", "gfl", "sfl", "gw" };

static void NameOtherThread(int site) {
    void* tls = getThreadLocalStorage();
    for (int i = 0; i < sOthNamedN; i++) {
        if (sOthNamed[i] == tls) {
            return;
        }
    }
    if (sOthNamedN >= OTH_NAMED_MAX) {
        return;
    }
    sOthNamed[sOthNamedN++] = tls;
    u32 id = 0;
    s32 prio = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    GxcLog("[gxcost] oth thread id=%lu prio=0x%lx first site=%s", (unsigned long)id, (unsigned long)prio, kSiteName[site]);
}

static void End(int site, u32 t0, u32 bytes) {
    if (!sOn) {
        return;
    }
    const int cls = ThreadClass();
    if (cls == THR_OTH) {
        NameOtherThread(site);
    }
    ThreadRow* r = &sNow.row[cls];
    r->ticks[site] += (u32)svcGetSystemTick() - t0;
    r->calls[site]++;
    r->bytes[site] += bytes;
}

static void NoteEventThread(int evt) {
    if (sEvTls == NULL) {
        sEvTls = getThreadLocalStorage();
    }
    if (evt >= 0 && evt < IRQ_SLOTS) {
        sNow.irq[evt]++;
    }
}

void gdx3ds_gxcost_on_event(int evt) {
    if (sOn) {
        NoteEventThread(evt);
    }
}

static void OnOwnEvent(void* data) {
    NoteEventThread((int)(uintptr_t)data);
}

static void RegisterCallbacks(void) {
    const int gputlOwns = &gdx3ds_gputl_on != NULL && gdx3ds_gputl_on() != 0;
    if (!gputlOwns) {
        gspSetEventCallback(GSPGPU_EVENT_PSC0, OnOwnEvent, (void*)(uintptr_t)GSPGPU_EVENT_PSC0, false);
        gspSetEventCallback(GSPGPU_EVENT_PPF, OnOwnEvent, (void*)(uintptr_t)GSPGPU_EVENT_PPF, false);
        gspSetEventCallback(GSPGPU_EVENT_P3D, OnOwnEvent, (void*)(uintptr_t)GSPGPU_EVENT_P3D, false);
    }
    gspSetEventCallback(GSPGPU_EVENT_PSC1, OnOwnEvent, (void*)(uintptr_t)GSPGPU_EVENT_PSC1, false);
    gspSetEventCallback(GSPGPU_EVENT_DMA, OnOwnEvent, (void*)(uintptr_t)GSPGPU_EVENT_DMA, false);
    GxcLog("[gxcost] init (debug.gxcost=1: GX/GSP call CPU per thread, event-thread wakes, GX irqs; ~1 s receipt; "
           "PSC0/PPF/P3D callbacks %s)", gputlOwns ? "via gputl" : "own");
}

static void Latch(void) {
    if (sLatched || !gdx3ds_config_loaded()) {
        return;
    }
    sLatched = 1;
    if (gdx3ds_config_get_int("debug", "gxcost", 0) == 0) {
        return;
    }
    memset(&sNow, 0, sizeof(sNow));
    memset(&sPrev, 0, sizeof(sPrev));
    sWinStart = svcGetSystemTick();
    sOn = 1;
    RegisterCallbacks();
}

static int CurrentArm(void) {
    return &gdx3ds_stereo_onesub_arm != NULL ? gdx3ds_stereo_onesub_arm() : -1;
}

/* "n/us" (and "/KB") per frame of one call site, delta since the previous receipt. */
static int AppendSite(char* buf, int at, int cap, const char* name, const ThreadRow* now, const ThreadRow* prev, int site,
                      float inv, int withBytes) {
    const u32 calls = now->calls[site] - prev->calls[site];
    if (calls == 0 || at >= cap) {
        return at;
    }
    const float us = (float)(now->ticks[site] - prev->ticks[site]) / (float)CPU_TICKS_PER_USEC;
    int w;
    if (withBytes) {
        const float kb = (float)(now->bytes[site] - prev->bytes[site]) / 1024.0f;
        w = snprintf(buf + at, (size_t)(cap - at), " %s=%.1f/%.0f/%.0f", name, (double)((float)calls * inv),
                     (double)(us * inv), (double)(kb * inv));
    } else {
        w = snprintf(buf + at, (size_t)(cap - at), " %s=%.1f/%.0f", name, (double)((float)calls * inv), (double)(us * inv));
    }
    return w > 0 ? at + w : at;
}

static void Emit(void) {
    Counters snap;
    memcpy(&snap, (const void*)&sNow, sizeof(snap));
    const u32 frames = snap.frames - sPrev.frames;
    if (frames == 0) {
        return;
    }
    const float inv = 1.0f / (float)frames;
    const ThreadRow* mn = &snap.row[THR_MAIN];
    const ThreadRow* mp = &sPrev.row[THR_MAIN];
    char buf[480];
    const int cap = (int)sizeof(buf);
    int at = snprintf(buf, sizeof(buf), "[gxcost] f=%lu arm=%d ent/f=%.2f | main us/f fb=%.0f fe=%.0f", (unsigned long)frames,
                      sWinArm, (double)((float)(snap.entries - sPrev.entries) * inv),
                      (double)((float)(mn->ticks[GXC_FB] - mp->ticks[GXC_FB]) / (float)CPU_TICKS_PER_USEC * inv),
                      (double)((float)(mn->ticks[GXC_FE] - mp->ticks[GXC_FE]) / (float)CPU_TICKS_PER_USEC * inv));
    at = AppendSite(buf, at, cap, "qw", mn, mp, GXC_QW, inv, 0);
    at = AppendSite(buf, at, cap, "qr", mn, mp, GXC_QR, inv, 0);
    at = AppendSite(buf, at, cap, "pcl", mn, mp, GXC_PCL, inv, 0);
    at = AppendSite(buf, at, cap, "sub", mn, mp, GXC_SUB, inv, 0);
    at = AppendSite(buf, at, cap, "gfl", mn, mp, GXC_GFL, inv, 1);
    at = AppendSite(buf, at, cap, "sfl", mn, mp, GXC_SFL, inv, 1);
    at = AppendSite(buf, at, cap, "gw", mn, mp, GXC_GW, inv, 0);
    if (at < cap) {
        const int w = snprintf(buf + at, (size_t)(cap - at), " | ev wake/f=%.2f us/f=%.0f",
                               (double)((float)(snap.evWakes - sPrev.evWakes) * inv),
                               (double)((float)(snap.evBusy - sPrev.evBusy) / (float)CPU_TICKS_PER_USEC * inv));
        at = w > 0 ? at + w : at;
    }
    at = AppendSite(buf, at, cap, "sub", &snap.row[THR_EV], &sPrev.row[THR_EV], GXC_SUB, inv, 0);
    if (at < cap) {
        const int w = snprintf(buf + at, (size_t)(cap - at), " | oth");
        at = w > 0 ? at + w : at;
    }
    at = AppendSite(buf, at, cap, "sub", &snap.row[THR_OTH], &sPrev.row[THR_OTH], GXC_SUB, inv, 0);
    at = AppendSite(buf, at, cap, "gfl", &snap.row[THR_OTH], &sPrev.row[THR_OTH], GXC_GFL, inv, 1);
    at = AppendSite(buf, at, cap, "sfl", &snap.row[THR_OTH], &sPrev.row[THR_OTH], GXC_SFL, inv, 1);
    if (at < cap) {
        const int w = snprintf(buf + at, (size_t)(cap - at), " | irq/f p3d=%.2f ppf=%.2f psc0=%.2f psc1=%.2f dma=%.2f",
                               (double)((float)(snap.irq[GSPGPU_EVENT_P3D] - sPrev.irq[GSPGPU_EVENT_P3D]) * inv),
                               (double)((float)(snap.irq[GSPGPU_EVENT_PPF] - sPrev.irq[GSPGPU_EVENT_PPF]) * inv),
                               (double)((float)(snap.irq[GSPGPU_EVENT_PSC0] - sPrev.irq[GSPGPU_EVENT_PSC0]) * inv),
                               (double)((float)(snap.irq[GSPGPU_EVENT_PSC1] - sPrev.irq[GSPGPU_EVENT_PSC1]) * inv),
                               (double)((float)(snap.irq[GSPGPU_EVENT_DMA] - sPrev.irq[GSPGPU_EVENT_DMA]) * inv));
        at = w > 0 ? at + w : at;
    }
    const size_t len = at < cap ? (size_t)at : (size_t)cap - 1;
    svcOutputDebugString(buf, len);
    gdx3ds_filelog_write(buf, len);
    sPrev = snap;
}

/* After a frame's C3D_FrameEnd: count it, and emit once a second or when the A/B arm changes. */
static void FrameDone(void) {
    const gxCmdQueue_s* q = (const gxCmdQueue_s*)(const void*)__C3D_Context;
    const int arm = CurrentArm();
    if (arm != sWinArm) {
        Emit(); /* the frames so far ran under the previous arm */
        sWinArm = arm;
        sWinStart = svcGetSystemTick();
    }
    sNow.frames++;
    sNow.entries += q->numEntries;
    const u64 now = svcGetSystemTick();
    if (now - sWinStart >= (u64)SYSCLOCK_ARM11) {
        Emit();
        sWinStart = now;
    }
}

/* ---------------------------------------------------------------------------------------------
 * The wrappers. Off: the real call behind one load + branch.
 * --------------------------------------------------------------------------------------------- */

Result __wrap_GSPGPU_FlushDataCache(const void* adr, u32 size) {
    const u32 t0 = Begin();
    const Result r = __real_GSPGPU_FlushDataCache(adr, size);
    End(GXC_GFL, t0, size);
    return r;
}

Result __wrap_svcFlushProcessDataCache(Handle process, u32 addr, u32 size) {
    const u32 t0 = Begin();
    const Result r = __real_svcFlushProcessDataCache(process, addr, size);
    End(GXC_SFL, t0, size);
    return r;
}

Result __wrap_gspSubmitGxCommand(const u32 gxCommand[0x8]) {
    const u32 t0 = Begin();
    const Result r = __real_gspSubmitGxCommand(gxCommand);
    End(GXC_SUB, t0, 0);
    return r;
}

Result __wrap_GX_ProcessCommandList(u32* buf0a, u32 buf0s, u8 flags) {
    const u32 t0 = Begin();
    const Result r = __real_GX_ProcessCommandList(buf0a, buf0s, flags);
    End(GXC_PCL, t0, 0);
    return r;
}

void __wrap_gxCmdQueueRun(gxCmdQueue_s* queue) {
    const u32 t0 = Begin();
    __real_gxCmdQueueRun(queue);
    End(GXC_QR, t0, 0);
}

bool __wrap_gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout) {
    const u32 t0 = Begin();
    const bool r = __real_gxCmdQueueWait(queue, timeout);
    End(GXC_QW, t0, 0);
    return r;
}

void __wrap_gspWaitForEvent(GSPGPU_Event id, bool nextEvent) {
    const u32 t0 = Begin();
    __real_gspWaitForEvent(id, nextEvent);
    End(GXC_GW, t0, 0);
}

/* The event thread's busy time: from the return of its wait (a wake) to its next wait. Only the
 * event thread pays more than a TLS compare here, and only once it has been identified. */
Result __wrap_svcWaitSynchronization(Handle handle, s64 nanoseconds) {
    const int ev = sOn && sEvTls != NULL && getThreadLocalStorage() == sEvTls;
    if (ev && sEvAwake) {
        sNow.evBusy += (u32)svcGetSystemTick() - sEvWakeTick;
        sEvAwake = 0;
    }
    const Result r = __real_svcWaitSynchronization(handle, nanoseconds);
    if (ev) {
        sEvWakeTick = (u32)svcGetSystemTick();
        sEvAwake = 1;
        sNow.evWakes++;
    }
    return r;
}

bool __wrap_C3D_FrameBegin(u8 flags) {
    if (!sLatched) {
        Latch();
    }
    sMainTls = getThreadLocalStorage(); /* the host loop, or the Old 3DS tick-ahead pipe's render side */
    const u32 t0 = Begin();
    const bool r = __real_C3D_FrameBegin(flags);
    End(GXC_FB, t0, 0);
    return r;
}

void __wrap_C3D_FrameEnd(u8 flags) {
    const u32 t0 = Begin();
    __real_C3D_FrameEnd(flags);
    End(GXC_FE, t0, 0);
    if (sOn) {
        FrameDone();
    }
}
