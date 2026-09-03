/* port/gdx_fiber.h -- cooperative context (fiber) abstraction.
 *
 * The cooperative scheduler (port/n64_sched.c) runs the decomp's real N64 threads as
 * cooperative contexts on a single OS thread. Win32 fibers and POSIX ucontext_t have no
 * common API, so this header is the seam that lets n64_sched.c stay platform-neutral.
 *
 * INVARIANTS (both backends):
 *   - Every call here must happen on the scheduler-owning thread -- the one that
 *     called gdx_fiber_convert_thread(). Switching from any other OS thread
 *     corrupts or crashes the scheduler (this is why the audio-thread affinity
 *     guard in n64_sched.c exists).
 *   - A GdxFiber entry function is not expected to return: the decomp threads
 *     end via __osCleanupThread(), which reschedules to another context. If an
 *     entry ever does return, the Win32 backend falls through its trampoline and
 *     the ucontext backend aborts (uc_link is NULL by design -- there is nowhere
 *     to return to).
 */
#ifndef GDX_FIBER_H
#define GDX_FIBER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxFiber GdxFiber; /* opaque */
typedef void (*GdxFiberEntry)(void* arg);

/* Call once, before any gdx_fiber_create/gdx_fiber_switch. Returns the host GdxFiber — the
 * target to switch back to when the run queue drains. */
GdxFiber* gdx_fiber_convert_thread(void);

/* Runs entry(arg) on the first switch, not here. stackSize==0 selects a 1 MB default.
 * Returns NULL on allocation failure. */
GdxFiber* gdx_fiber_create(GdxFiberEntry entry, void* arg, size_t stackSize);

/* Save the currently running context and resume `to`. */
void gdx_fiber_switch(GdxFiber* to);

/* Used only by the scheduler's audio-thread affinity guard, so an opaque-but-stable-per-thread
 * value is sufficient — the backends return whatever their platform makes cheap. */
unsigned long gdx_fiber_current_thread_id(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_FIBER_H */
