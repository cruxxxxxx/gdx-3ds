/* port/3ds/include/gdx3ds_renderthread.h -- render thread on core 2 (LOCKED-60 Task H).
 *
 * Design: docs/research/renderthread-audit.md. The game's own frame protocol is an intra-
 * iteration fork/join: osSpTaskStartGo hands the GFX task to this thread and posts SP-done;
 * the host loop, once the game fibers are all blocked, waits for the task and posts DP-done
 * (the game's "previous frame rendered" fence), then re-dispatches. Everything that touches
 * citro3d/GX (StartFrame, gdx_gfx_run, the VI-fallback present, EndFrame) executes on the
 * render thread; the main thread paces on vblank and never opens a C3D frame itself.
 *
 * Killswitch `[debug] renderthread` (gdiffuser.ini): 0 = off (no thread, no hooks: the
 * sequential path is byte-identical), 1 = on. Mode reported by gdx3ds_rt_mode():
 *   0 off, 1 sync (M2: every command is waited for immediately -- proves core-2 GPU
 *   submission with zero concurrency), 2 pipe (M3: TASK/END asynchronous, DP-done at the join).
 * `[debug] renderthread_sync = 1` forces mode 1 for A/B.
 *
 * Threading contract: the render thread is prio 0x24 on core 2 (below the 0x18 audio threads,
 * which the strict-priority kernel must always favor), 192 KiB stack, blocks on a
 * LightSemaphore between commands; waiters block on a sticky LightEvent re-armed per wait.
 * Nothing spins. All main-side entry points are main-thread-only (game fibers included). */
#ifndef GDX3DS_RENDERTHREAD_H
#define GDX3DS_RENDERTHREAD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxRtCallbacks {
    void (*startFrame)(void);      /* Fast3dWindow::StartFrame (opens the C3D frame) */
    void (*presentFallback)(void); /* gdx_vi_present_fallback (fb0 bind / hold / VI quad) */
    void (*endFrame)(void);        /* Fast3dWindow::EndFrame (C3D_FrameEnd + window swap) */
} GdxRtCallbacks;

/* Reads the ini, creates the thread. Returns the mode (0 = sequential path). Logs `[rt] ...`. */
int gdx3ds_rt_init(const GdxRtCallbacks* cb);
int gdx3ds_rt_mode(void);

/* ---- per-iteration protocol (main thread) ---- */
void gdx3ds_rt_join_idle(void);    /* loop top / APT / teardown: wait until the ring is drained */
int gdx3ds_rt_idle(void);          /* 1 when no command is queued or running */
void gdx3ds_rt_pace_vblank(void);  /* adaptive vblank wait (same policy as gpuprof's SYNCDRAW skip) */
void gdx3ds_rt_frame_begin(void);  /* BEGIN: StartFrame on the render thread */
void gdx3ds_rt_frame_end(void);    /* END: presentFallback + EndFrame on the render thread */

/* Game-fiber entry (osSpTaskStartGo, n64_sched.c). Returns 0 when rt is off (caller renders
 * inline as before), 1 when the task was executed to completion (sync mode: caller posts SP and
 * DP-done as before), 2 when it is in flight (pipe mode: caller posts SP-done only; the host
 * loop posts DP-done through gdx3ds_rt_post_dp_done after gdx3ds_rt_wait_task). */
int gdx3ds_rt_submit_task(void* dl, size_t dlSize, int ucode);
int gdx3ds_rt_task_pending(void);  /* a TASK is queued or running (pipe mode) */
void gdx3ds_rt_wait_task(void);    /* block until the in-flight TASK completed (no DP post) */
void gdx3ds_rt_post_dp_done(void); /* defined in n64_sched.c: osSendMesg(&D_800DCAC8, 0x2A) */

/* Any thread: no-op on the render thread / when idle; otherwise waits for the in-flight
 * TASK. Bridge entry points that mutate walk-visible tables from the game thread call this
 * first (audit section 4). */
void gdx3ds_rt_fence(void);
int gdx3ds_rt_on_render_thread(void);

/* Telemetry on the verbose cadence: `[rt] mode= n= tasks= waitMain= waitRender= fence= ...`. */
void gdx3ds_rt_emit_receipt(unsigned long frame);

/* Teardown: QUIT + threadJoin. Safe when never initialized. */
void gdx3ds_rt_shutdown(void);

/* Console-echo mute for the calling thread (port_log.h / GFX_C3D_LOG honor it): the libctru
 * console is not thread-safe, so render-thread diagnostics go to svc + filelog only. */
extern __thread int gdx_port_log_console_muted;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_RENDERTHREAD_H */
