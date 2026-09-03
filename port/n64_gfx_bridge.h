#ifndef GDIFFUSER_N64_GFX_BRIDGE_H
#define GDIFFUSER_N64_GFX_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GdxTaskUcode {
    GDX_TASK_UCODE_F3DEX2 = 0,
    GDX_TASK_UCODE_F3DLX2_REJ = 1,
    GDX_TASK_UCODE_F3DFLX2_REJ = 2,
} GdxTaskUcode;

void gdx_gfx_run(void* dl, size_t dlSize, GdxTaskUcode taskUcode);
/* RENDER THREAD (3DS): per-task private segment table for the walking thread (NULL = the live
   game array) and the host-side merge of walk-time zero-slot claims at the join. */
void gdx_gfx_segment_view_set(uintptr_t* view);
int gdx_gfx_segment_claims_merge(const uintptr_t* view);
void gdx_register_n64_framebuffer(void* cpuAddr, unsigned int width, unsigned int height);
void gdx_vi_set_next_framebuffer(void* cpuAddr);
void gdx_vi_set_current_framebuffer(void* cpuAddr);

/* Must be called once per host frame AFTER the game threads are dispatched and BEFORE
 * EndFrame. No-op when a real GFX task rendered; otherwise presents the current VI
 * framebuffer's CPU-written pixels as a textured quad, preserving N64 "VI scans out
 * whatever is in RDRAM" semantics (boot logo, etc.). */
void gdx_vi_present_fallback(void);

/* ======================================================================================
 * Main-loop render/logic decoupling (frame interpolation) host API.
 *
 * The retained display list and per-tick lerp scratch live only inside gdx_gfx_run (freed at
 * its tail; the GfxPool toggles on the next tick), so the M-sub-frame present loop has to run
 * there too. The HOST (port/main.cpp) owns pacing: it supplies the clock, configures the
 * schedule, then reads back what was presented. Inert unless
 * gEnhancements.Graphics.FrameInterpolation != 0 (or GDX_INTERP_P2) — see
 * gdx_interp::P2HostActive(); default OFF is the byte-identical single-pass path.
 * ==================================================================================== */

/* Monotonic wall-clock (seconds), sampled once per sub-frame to derive
 * t = clamp((now - tickStart)/tickDuration, 0, 0.999). */
typedef double (*GdxInterpNowFn)(void);
void gdx_gfx_interp_set_now_fn(GdxInterpNowFn fn);

/* 1 if the decoupled loop is active this process (CVar/env). Read ONCE per host iteration to
 * choose the interpolation present path over the single-present default. */
int gdx_gfx_interp_host_active(void);

/* Configure this tick's sub-frame schedule. Must be called BEFORE gdx_dispatch, since the game
 * fiber (and thus gdx_gfx_run) runs inside dispatch. active=0 forces the single-pass path.
 * tickStart/tickDuration are in the registered now-fn's clock units; maxSubframes caps the loop
 * when VSync is off and presents don't block. Resets the "presented" flag. */
void gdx_gfx_interp_tick_config(int active, double tickStart, double tickDuration, int maxSubframes);

/* 1 if gdx_gfx_run presented the frame(s) itself this tick (a real gfx task ran AND interp was
 * active). When 0 on an interp tick, the host must present once itself (taskless VI fallback). */
int gdx_gfx_interp_presented_last_tick(void);

/* Telemetry for the host's rate-limited [interp-p2] line. */
int gdx_gfx_interp_last_subframes(void);
// Sub-frames the swapchain limiter refused last tick. Non-zero means the tick overran its budget.
// Counted separately from presented ones, or every rate reading caps at a fiction.
int gdx_gfx_interp_last_dropped(void);
double gdx_gfx_interp_last_t(void);

/* GFX tasks (gdx_gfx_run calls) the previous tick submitted; normally 2-6. Load-bearing:
 * gdx_gfx_run runs PER TASK, so interpolation state that must roll once per TICK cannot be
 * rolled there — do that and each task tests its offsets against the previous TASK's set and
 * snaps instead of lerping. Reported so the distinction stays visible in a log. */
int gdx_gfx_interp_last_tasks(void);

/* [interp-pair] Pairing quality. Slot identity is the GfxPool BYTE OFFSET, valid only while the
 * pool layout is stable frame to frame -- and the pool fills in draw-submission order, so any
 * change in the visible set (track-chunk cull, objects entering/leaving view) shifts it and pairs
 * offset N against a DIFFERENT object than last tick. The 2000-unit teleport guard cannot catch
 * that: normal motion is a few tens of units and adjacent track chunks sit far closer than 2000
 * apart. These report the largest delta among slots that actually paired and how many paired
 * slots moved further than a tick plausibly can; a fat tail appearing exactly when the camera
 * sweeps is the signature of byte-offset identity being the defect.
 *
 * pair_max is the worst delta since the PREVIOUS telemetry line (read-and-reset); pair_susp and
 * pair_tot are CUMULATIVE since boot, so their ratio is the mispairing rate. Cumulative because
 * the line prints one tick in 120 -- a per-tick snapshot makes a low-rate fault statistically
 * invisible. */
float gdx_gfx_interp_pair_max_delta(void);
int gdx_gfx_interp_pair_suspect(void);
int gdx_gfx_interp_pair_lerped_total(void);

/* [interp-idem] Ticks whose sub-frame replays bound DIFFERENT textures than pass 0, over ticks
 * that replayed more than once. Non-zero means replaying one tick's display list is NOT
 * idempotent, which shows up as flicker. Candidate mechanism: mRdp->loaded_texture survives
 * Run() and StoreLoadedTexture (interpreter.cpp:4421) erases overlapping entries, so replay 2
 * starts from replay 1's end-state. Zero across a flickering race rules that out. */
int gdx_gfx_interp_idem_divergent(void);
int gdx_gfx_interp_idem_multipass(void);

#ifdef __cplusplus
}
#endif

#endif
