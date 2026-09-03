/* port/3ds/os/gdx_fiber_3ds.c -- libctru-thread fiber backend for port/gdx_fiber.h (stream B).
 *
 * The 3DS has no ucontext and no fiber primitive, so each GdxFiber is a real libctru
 * thread parked on a LightEvent. Cooperation is enforced by construction: exactly one
 * fiber's event is ever signaled at a time -- gdx_fiber_switch signals the target's event
 * and then blocks on its own, so at most one fiber thread is runnable, which preserves the
 * "single logical scheduler thread" model n64_sched.c assumes.
 *
 * CORE:     every fiber thread is created on the SAME core as the thread that called
 *           gdx_fiber_convert_thread (appcore/core 0 for a .3dsx main thread; queried via
 *           svcGetProcessorID rather than hardcoded). Same-core is deliberate: it removes
 *           any possibility of true parallelism between "fibers" (matching the Win32/
 *           ucontext backends' semantics exactly) and sidesteps cross-core cache/timing
 *           surprises in decomp code that assumes cooperative scheduling. Cores 1-3 stay
 *           free for stream C's audio thread.
 *
 * PRIORITY: main-thread priority + 1 (numerically higher = LOWER priority on 3DS; a
 *           typical app main thread is 0x30, so fibers land at 0x31, clamped to the 0x3F
 *           floor). Because parked fibers are blocked, not spinning, priority is mostly
 *           moot in normal operation -- the strictly-lower priority is a safety property:
 *           if a bug ever leaves the scheduler thread AND a fiber runnable simultaneously,
 *           the scheduler thread wins the core, keeping APT/vblank pumping.
 *
 * STACK:    gdx_fiber_create's stackSize is passed straight to threadCreate (rounded up to
 *           8-byte alignment as threadCreate requires); 0 selects the 1 MB default the
 *           gdx_fiber.h contract promises. CAUTION for integration: the 3DS has no demand
 *           paging -- threadCreate heap-allocates the full stack up front, so N fibers cost
 *           N real megabytes. If memory gets tight, pass explicit sizes from n64_sched.c
 *           (contract change filed in STATUS.md as a watch item, not needed yet).
 *
 * THREAD ID / AFFINITY GUARD: n64_sched.c records gdx_fiber_current_thread_id() once at
 * init and later compares against it from INSIDE game fibers to detect calls leaking in
 * from the audio thread. On Win32/ucontext all fibers share one OS thread so raw thread
 * ids work; here every fiber is its own OS thread, so returning raw ids would trip the
 * guard on every legitimate call (n64_sched.c:259 would spin-yield forever). Therefore
 * this backend returns the LOGICAL scheduler thread id -- the converted thread's real id --
 * for the host thread and every fiber thread it created (tracked via the sSelf TLS slot),
 * and the real OS thread id (svcGetThreadId) for foreign threads such as audio. That is
 * exactly the distinction the guard exists to make, and the header explicitly allows any
 * "opaque-but-stable" value.
 *
 * Invariants honored (see port/gdx_fiber.h):
 *   - entry functions never return: the trampoline abort()s if one does (ucontext parity)
 *   - no destroy exists: fiber threads and their stacks are immortal by design
 *   - all create/switch calls happen on the logical scheduler thread
 */
#include "gdx_fiber.h"

#include <3ds.h>
#include <stdlib.h>

#define GDX_FIBER_DEFAULT_STACK (1024u * 1024u) /* 1 MB, per the gdx_fiber.h contract */
#define GDX_FIBER_MIN_PRIO 0x3F                 /* numerically-largest valid = lowest priority */

struct GdxFiber {
    Thread thread;     /* NULL for the converted host thread */
    LightEvent resume; /* signaled -> this fiber owns the core until it switches away */
    GdxFiberEntry entry;
    void* arg;
    int isHost;
};

/* Which fiber is running on THIS OS thread (set once per thread, at convert/trampoline
 * time). Non-NULL marks the thread as part of the scheduler's cooperative group. */
static __thread GdxFiber* sSelf = NULL;

static u32 sHostThreadId = 0; /* real OS id of the converted thread = the logical id */
static s32 sFiberCore = 0;    /* captured in convert_thread; see CORE above */
static s32 sFiberPrio = 0x31; /* captured in convert_thread; see PRIORITY above */

static void gdx_fiber_3ds_trampoline(void* param) {
    GdxFiber* f = (GdxFiber*) param;
    sSelf = f;
    /* Park until the first gdx_fiber_switch to this fiber. No race with an early signal:
     * the event is ONESHOT, so a signal delivered before this wait simply satisfies it. */
    LightEvent_Wait(&f->resume);
    f->entry(f->arg);
    /* Entry is not expected to return (decomp threads exit via __osCleanupThread, which
     * reschedules). There is no context to fall back to -- abort loudly (ucontext parity)
     * rather than let threadExit silently strand whoever was supposed to run next. */
    abort();
}

GdxFiber* gdx_fiber_convert_thread(void) {
    GdxFiber* f = (GdxFiber*) calloc(1, sizeof(*f));
    s32 prio = 0x30;
    if (f == NULL) {
        return NULL;
    }
    f->isHost = 1;
    LightEvent_Init(&f->resume, RESET_ONESHOT);

    svcGetThreadId(&sHostThreadId, CUR_THREAD_HANDLE);
    sFiberCore = svcGetProcessorID();
    if (R_SUCCEEDED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE))) {
        sFiberPrio = (prio < GDX_FIBER_MIN_PRIO) ? prio + 1 : GDX_FIBER_MIN_PRIO;
    }

    sSelf = f; /* the calling thread IS this fiber right now */
    return f;
}

GdxFiber* gdx_fiber_create(GdxFiberEntry entry, void* arg, size_t stackSize) {
    GdxFiber* f;

    if (stackSize == 0) {
        stackSize = GDX_FIBER_DEFAULT_STACK;
    }
    stackSize = (stackSize + 7u) & ~(size_t) 7u; /* threadCreate wants 8-byte alignment */

    f = (GdxFiber*) calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    LightEvent_Init(&f->resume, RESET_ONESHOT);
    f->entry = entry;
    f->arg = arg;

    /* Not detached: the Thread handle stays valid forever, matching the no-destroy
     * contract. The thread starts immediately but parks in the trampoline. */
    f->thread = threadCreate(gdx_fiber_3ds_trampoline, f, stackSize, sFiberPrio, sFiberCore, false);
    if (f->thread == NULL) {
        free(f);
        return NULL;
    }
    return f;
}

void gdx_fiber_switch(GdxFiber* to) {
    GdxFiber* from = sSelf;
    /* Order matters for the no-lost-wakeup argument, not for races: signal the target
     * first (ONESHOT latches if it has not parked yet), then block ourselves. Between the
     * two calls both threads are briefly runnable; the kernel may even run `to` before we
     * park -- harmless, because if `to` switches straight back, OUR event latches and the
     * wait below falls through immediately. */
    LightEvent_Signal(&to->resume);
    LightEvent_Wait(&from->resume);
}

unsigned long gdx_fiber_current_thread_id(void) {
    u32 id = 0;
    if (sSelf != NULL) {
        /* Host thread or a fiber it created: report the logical scheduler id (see header
         * comment -- raw per-fiber OS ids would break n64_sched.c's affinity guard). */
        return (unsigned long) sHostThreadId;
    }
    /* Foreign thread (e.g. stream C's audio thread): its real, stable OS id. */
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return (unsigned long) id;
}
