/* port/3ds/gfx/gdx3ds_gpu_prof.h — G-GPUPROF: per-frame GPU-side timing telemetry
 * for the 60fps campaign (docs/research/60fps-research.md: the AA/fill-rate finding
 * says PICA fill cost, not game logic, was the SM64 60fps blocker — these counters
 * are how this port proves/disproves that for F-Zero X).
 *
 * Gate: gdiffuser.ini `[debug] gputrace = 1` (stream B's gdx3ds_config, read once,
 * weak-linked so the DL-test harness — which does not link gdx3ds_os — compiles with
 * the gate permanently off). With the gate off every entry point is a branch on a
 * cached bool plus, for frame_begin, the exact C3D_FrameBegin(C3D_FRAME_SYNCDRAW)
 * call the backend made before this TU existed.
 *
 * Output (svc debug channel, style-matched to the [c3d]/[present] lines of
 * docs/research/m1-boot-debug.md — bounded, every 64th frame ≈ 1 line/s):
 *
 *   [gpu]  frame=N n=64 wall=… build=… proc=… gpu=… wVbl=… wP3D=… cmd=…% draws=… tris=…
 *          imp=… rl=… rm=… md=…   (SELECT-PERF columns, see below)
 *   [fill] frame=N n=64 passes=… (scr=… tex=…) copies=… rdbk=… estMpix=…
 *          texBytes=… uniqueTex=… vtx=…   (S12 asset-cost columns, per-frame avgs)
 *
 * Column semantics (all ms, window averages; verified against the shipped
 * libcitro3d disassembly — see the .c):
 *   wall   time between consecutive C3D_FrameEnd exits (whole frame incl. logic+waits)
 *   build  StartFrame(fresh)→EndFrame-entry: CPU building the frame (interpreter+backend)
 *   proc   C3D_GetProcessingTime(): citro3d's FrameBegin→FrameEnd CPU window
 *          (≈ build + the FrameEnd linear-heap flush/submit)
 *   gpu    C3D_GetDrawingTime(): GX queue execution of the previously submitted
 *          frame, submission → onQueueFinish (P3D draw + PPF display transfer)
 *   wVbl   time blocked in C3D_FrameSync (vblank pacing wait)
 *   wP3D   time blocked in C3D_FrameBegin's gxCmdQueueWait (GPU still busy with the
 *          previous frame — THE "GPU-bound" signal)
 *   cmd    C3D_GetCmdBufUsage() max over the window (3D command buffer pressure)
 *   imp    texture-import lookups (Interpreter::ImportTexture) — DELTA over the
 *          window's n frames (per-frame rate = imp/n); same counter as [c3d] dImp,
 *          duplicated here so one [gpu] line correlates cost with import volume
 *   rl     full o2r SETTIMG resource resolutions (LoadResourceProcess taken) — delta
 *   rm     SETTIMG-memo hits (resolution skipped) — delta; rl≈0 with rm carrying the
 *          volume means the SELECT-PERF memo is absorbing the per-frame lookups
 *   md     game screen at emit time: decomp GET_MODE(gGameMode) (7=main menu,
 *          8=machine select, 9=machine settings/accel-maxspeed, 10=course select;
 *          -1 pre-mode, -2 symbol unavailable) — correlates windows to screens
 *
 * A SHOT label (B2's deterministic-tick screenshot mechanism, port/input_bridge.c →
 * gdx_request_frame_dump) additionally emits immediate single-frame
 * `[gpu] tag=<label> …` / `[fill] tag=<label> …` lines so scripted runs get
 * scene-tagged numbers at title/menu/race checkpoints.
 */
#ifndef GDX3DS_GPU_PROF_H
#define GDX3DS_GPU_PROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame lifecycle. frame_begin OWNS the backend's C3D_FrameBegin call so the vblank
 * wait (C3D_FrameSync) and the GPU-queue drain wait can be timed separately;
 * reentry != 0 reproduces the original re-entrant StartFrame behavior exactly
 * (C3D_FrameBegin early-outs on inFrame before ever looking at SYNCDRAW). */
void gdx3ds_gpuprof_frame_begin(int reentry);
/* Immediately before / after C3D_FrameEnd(0) in EndFrame. */
void gdx3ds_gpuprof_frame_end_pre(void);
void gdx3ds_gpuprof_frame_end_post(unsigned frameDraws, unsigned frameTris);

/* Fill-rate attribution: one pass per render-target bind (StartDrawToFramebuffer),
 * targetPixels = the bound target's full extent (approximation per the fill-suspect
 * model: VI-mirror composite / transition captures / stereo double-pass are
 * full-target fills). The fb0 pass implied by every fresh frame_begin is counted
 * internally. Copies (GX texture copy) and readbacks (GX display transfer +
 * CPU un-rotation) are tracked separately from draw-pass fill. */
void gdx3ds_gpuprof_note_pass(int texBacked, unsigned targetPixels);
void gdx3ds_gpuprof_note_copy(unsigned pixels);
void gdx3ds_gpuprof_note_readback(unsigned pixels);

/* Asset-cost telemetry for the 60fps campaign S12 gate (docs/research/
 * 60fps-campaign-plan.md): the three columns that decide whether ETC1 conversion
 * (S12a) or geometry decimation (S12b) would pay. Folded into the [fill] line
 * alongside the fill-rate columns.
 *   - note_tex_upload(bytes): a successful UploadTexture; `bytes` is the swizzled
 *     PICA texture payload flushed to the GPU (t.tex.size — the real upload cost,
 *     post pow2-pad). Accumulated per frame → texBytes column (upload/swizzle proxy).
 *   - note_tex_bind(texId): a texture bound for a draw this frame; the TU dedupes
 *     per frame → uniqueTex column (distinct textures touched, the resident-set
 *     proxy for the ETC1 memory argument).
 *   - note_verts(count): vertices pushed through the repack/transform pipe in one
 *     DrawTriangles. Accumulated per frame → vtx column (decimation proxy). */
void gdx3ds_gpuprof_note_tex_upload(unsigned bytes);
void gdx3ds_gpuprof_note_tex_bind(unsigned texId);
void gdx3ds_gpuprof_note_verts(unsigned count);

/* Scene tag: the next frame_end_post emits immediate single-frame [gpu]/[fill]
 * lines carrying tag=<label>. Called (weak-linked) from gdx_request_frame_dump so
 * every scripted SHOT doubles as a telemetry checkpoint. */
void gdx3ds_gpuprof_note_shot(const char* label);

/* FPS-HUD sample (port/3ds/gdx3ds_fps_hud.c): the current — possibly partial —
 * window's per-frame CPU-build average and the [profop] opcode with the most
 * accumulated ticks so far in the window. Returns 1 and fills the out-params
 * when gputrace is armed and at least one frame has folded since the last
 * window emit; returns 0 (outputs untouched) otherwise. Values are integer
 * TENTHS of a millisecond (the HUD's refresh path may not format floats —
 * newlib's %f can malloc via dtoa). *outTopOp is the opcode byte, or -1 when
 * no opcode ticks have accumulated yet. Read-only: nothing is reset, the
 * [gpu]/[profop] window emits are unaffected. Render thread only, like every
 * other entry point here. */
int gdx3ds_gpuprof_hud_sample(unsigned* outBuildMsX10, int* outTopOp, unsigned* outTopOpMsX10);

/* MENU DBG tab: live gputrace arm/disarm (render/main thread, between frames only)
 * + current state. set_enabled also finalizes the config latch so the boot-time
 * INI value can no longer overwrite a user flip. */
void gdx3ds_gpuprof_set_enabled(int on);
int gdx3ds_gpuprof_get_enabled(void);

/* RENDER THREAD (gdx3ds_renderthread.cpp): when on, a fresh frame_begin never runs
 * C3D_FrameSync -- the main thread paces on vblank itself and the render thread's
 * C3D_FrameBegin(0) keeps only the gxCmdQueueWait backpressure ([gpu] wP3D). */
void gdx3ds_gpuprof_set_external_pacing(int on);

/* ------------------------------------------------------------------------------
 * [prof] — sectioned CPU-build profiler (BUILD-PROFILER). The race is decisively
 * CPU-bound (measurement-2026-08-20: gpu flat 0.4 ms, wP3D 0.0, build 24-70 ms),
 * so this attributes the BUILD milliseconds to the frame path's major phases:
 *
 *   BR  bridge ProcessList pre-pass (N64DisplayListAdapter::ConvertRoot walk)
 *   RUN Interpreter::Run SELF time — dispatch loop + every handler NOT below
 *   VTX GfxSpVertex (per-vertex matrix transform + lighting + fog bake)
 *   TRI GfxSpTri1 (state resolve + clip + repack into bufVbo; incl. rect tris)
 *   IMP ImportTexture (lookup + decode; child of TRI's state resolve)
 *   DRW backend DrawTriangles (vertex repack loop + state application + C3D)
 *   MTX GfxSpMatrix (matrix load/multiply/stack)
 *
 * All buckets are EXCLUSIVE (self) time: a section's ticks exclude any profiled
 * section nested inside it (gdx3ds_prof_child_ticks carries child time up to the
 * enclosing scope), so the buckets never double-count and they sum. RUN self time
 * IS the dispatch remainder the campaign asked for. Everything runs on the render
 * thread (same thread as the game fibers) — no locking, raw tick reads only, no
 * division until emit. Gate: the same latched debug.gputrace (gdx3ds_prof_active
 * is 0 unless tracing, and every hook site is one int load + branch when off).
 *
 * Emitted as a [prof] companion line to [gpu] (same 64-frame cadence), plus a
 * [prof!] line covering ONLY the window's frames with draws > 90 (the 30-machine
 * race-start storm) so race-start separates from steady-state inside one window:
 *
 *   [prof]  frame=N n=64 br= dsp= vtx= tri= imp= drw= mtx= oth= | vtxN= nV= nT=
 *           nI= nD= nM= nRun=
 *   [prof!] frame=N n=<stormFrames> ... (identical columns, storm frames only)
 *
 * Columns (per-frame window averages, ms): br/vtx/tri/imp/drw/mtx as above; dsp =
 * RUN self (dispatch + un-instrumented handlers); oth = proc − (dsp+vtx+tri+imp+
 * drw+mtx) = the C3D CPU window minus the interpreter, i.e. ImGui/present glue/
 * post-Run bridge work. br sits OUTSIDE proc (the pre-pass runs before the fresh
 * C3D_FrameBegin inside Run's StartFrame) so it is not part of oth's subtraction.
 * Counts: vtxN = vertices loaded/frame; nV/nT/nI/nD/nM/nRun = calls/frame into
 * VTX/TRI/IMP/DRW/MTX/RUN. */
enum {
    GDX3DS_PROF_BR = 0,
    GDX3DS_PROF_RUN,
    GDX3DS_PROF_VTX,
    GDX3DS_PROF_TRI,
    GDX3DS_PROF_IMP,
    GDX3DS_PROF_DRW,
    GDX3DS_PROF_MTX,
    GDX3DS_PROF_SEC_COUNT
};

/* 1 iff gputrace latched on (mirrors the TU's sEnabled at each fresh frame_begin).
 * Hot paths gate on this single int before touching any accumulator. */
extern int gdx3ds_prof_active;
/* Per-frame accumulators, written directly by the hook sites; folded into the
 * window and reset in frame_end_post (NOT frame_begin: the bridge pre-pass runs
 * before the fresh frame_begin and must land in the frame it feeds). */
extern uint64_t gdx3ds_prof_sec_ticks[GDX3DS_PROF_SEC_COUNT];
extern uint32_t gdx3ds_prof_sec_calls[GDX3DS_PROF_SEC_COUNT];
extern uint32_t gdx3ds_prof_vtx_loaded;
/* Exclusive-time plumbing: monotone sum of every closed section's wall ticks.
 * A scope snapshots it on entry; on exit, (now − snap) is child time to subtract,
 * and the scope reports its own full wall delta upward by setting snap + dt. */
extern uint64_t gdx3ds_prof_child_ticks;
/* svcGetSystemTick behind a plain function so hook sites (including libultraship
 * TUs that never include <3ds.h>) need no svc declaration. */
long long gdx3ds_prof_now(void);

static inline uint64_t gdx3ds_prof_enter(uint64_t* childSnap) {
    *childSnap = gdx3ds_prof_child_ticks;
    return (uint64_t)gdx3ds_prof_now();
}

static inline void gdx3ds_prof_exit(int sec, uint64_t t0, uint64_t childSnap) {
    const uint64_t dt = (uint64_t)gdx3ds_prof_now() - t0;
    gdx3ds_prof_sec_ticks[sec] += dt - (gdx3ds_prof_child_ticks - childSnap);
    gdx3ds_prof_sec_calls[sec]++;
    gdx3ds_prof_child_ticks = childSnap + dt;
}

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_GPU_PROF_H */
