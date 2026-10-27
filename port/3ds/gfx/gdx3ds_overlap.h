/* port/3ds/gfx/gdx3ds_overlap.h -- C5 Stage 4 "[overlap]": let the PICA run while the CPU is
 * still building the frame (docs/research/native/c5overlap-progress.md).
 *
 * Stock citro3d (1.7.1) serialises the frame: C3D_FrameBegin stops and clears the GX queue, every
 * C3D_FrameSplit / GX_* call of the frame only APPENDS to that stopped queue, and C3D_FrameEnd
 * starts it. The GPU therefore idles for the whole CPU build and then runs the frame's 3-4 ms of
 * work while the next frame's C3D_FrameBegin waits for it (the `[gpu] wP3D` column: ~2.5 ms per
 * frame on the Old 3DS with frameskip off, hw-rel2).
 *
 * Lever `[debug] overlap_kick` (default 0 = off, byte-identical frame sequence):
 *   at the first scene-category boundary of the frame whose bit is set in
 *   `[debug] overlap_kick_mask` (default MACH|HUD, the c5cat ids), submit the command list built
 *   so far (VBO tail flush + linear-heap flush + C3D_FrameSplit(GX_CMDLIST_FLUSH)) and START the
 *   GX queue, so the sky/track (and then the machines) draw while the CPU builds the rest.
 *   QUICKFIX: 1 = CADENCE (kick only in a frame whose render follows a rendered tick: frameskip
 *   off, steady D=1, an auto/fixed render with no skip before it; decided per frame from
 *   gdx3ds_frameskip_render_gap), 2 = ALWAYS (the pre-quickfix behaviour of 1, for A/B). At D>=2
 *   the previous frame's GPU work is long done, so a kick has no wait to hide and its whole-heap
 *   flushes are pure cost (hwprof-progress.md §10.2). Receipt `[overlap] cadence` every 1024
 *   armed frames: consec / kickf / kicks / cad_skip (frames the cadence refused) / gapmax.
 *   Before C3D_FrameEnd the queue is drained and stopped again, so FrameEnd's own flush, display
 *   transfer and swap bookkeeping run exactly as stock. Only on renderthread=0 (the Old 3DS).
 * `[debug] overlap_heapflush` (default 1): flush the whole linear heap at every kick, the same
 *   flush C3D_FrameEnd does. 0 flushes only the VBO tail and the command list (cheaper; only
 *   safe if every other CPU-written GPU buffer flushes itself).
 * `[debug] endflush_cmdlist` (default 0): C3D_FrameEnd(GX_CMDLIST_FLUSH) instead of the stock
 *   whole-linear-heap flush; the backend's in-frame splits then flush their own lists.
 *
 * All entry points run on the thread that owns the C3D frame (main, renderthread=0). */
#ifndef GDX3DS_OVERLAP_H
#define GDX3DS_OVERLAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the kick lever is latched on: the decomp's scene markers must be written even when the
 * [c5cat] clock is unarmed (port/n64_sched.c gdx_c5cat_armed). */
int gdx3ds_overlap_markers_armed(void);
/* n60tail: kicks so far in the frame being built (0 = its GX queue has not run yet; a head run,
 * gdx3ds_overlap_head_run, counts as one). */
int gdx3ds_overlap_frame_kicks(void);

/* Fresh C3D frame opened (StartFrame, first begin of the frame): latches the ini once, resets
 * the per-frame kick state. */
void gdx3ds_overlap_frame_begin(void);

/* A c5cat scene marker reached the owned walker (gdx_rt_glue.cpp, p=0x61). */
void gdx3ds_overlap_scene_marker(unsigned category);

/* stereo-race-fps [stereo] head_kick: start the frame's GX queue NOW, with no heap flush, no split
 * and no stereo chunk: what is queued (the frame's clears of both eyes and the closed pre-body list)
 * runs on the GPU while the CPU builds the next categories. The caller (the stereo ReplayEnter) has
 * just closed the open list and guarantees that nothing queued so far draws, so the GPU reads no
 * CPU-written memory but that (flushed) list. Counts as a kick for the frame end's drain + stop, the
 * split flags and gdx3ds_overlap_frame_kicks (sky_late refuses a frame whose queue ran), not for the
 * kick budget, the kicked categories or the stereo chunks. Only when the kick lever is armed and the
 * frame passes its cadence (the frames that kick). 1 = the queue runs now. */
int gdx3ds_overlap_head_run(void);

/* o3ds-cpucuts [debug] overlap_flush_ranges: a CPU writer of GPU-read linear memory that does NOT
 * flush its own range notes it here, so a range-flush kick still cleans it (gdx3ds_overlap.c).
 * c5overlap's flush audit found none today; the list resets at every kick and frame begin. */
void gdx3ds_overlap_note_dirty(const void* addr, unsigned bytes);

/* Right before C3D_FrameEnd: if this frame kicked, wait for the queue to drain and stop it. */
void gdx3ds_overlap_before_frame_end(void);

/* n60st3 ([debug] spike_ms): the latest frame end's drain wait in ticks (0 when it did not kick)
 * and, in *kicks, that frame's kicks. */
unsigned long long gdx3ds_overlap_last_drain_ticks(int* kicks);

/* Flags for the backend's own in-frame C3D_FrameSplit calls (frame mirror copy, readback).
 * GX_CMDLIST_FLUSH once this frame kicked (the queue is live, so the chunk goes to the GPU at
 * once, before C3D_FrameEnd's linear-heap flush) or under endflush_cmdlist; 0 otherwise (stock). */
unsigned gdx3ds_overlap_split_flags(void);

/* Flags for C3D_FrameEnd: GX_CMDLIST_FLUSH under `[debug] endflush_cmdlist=1` (flush the final
 * command list only, not the whole linear heap: every other CPU-written GPU buffer flushes
 * itself, see c5overlap-progress.md "flush audit"), 0 otherwise (stock). */
unsigned gdx3ds_overlap_frame_end_flags(void);

/* C6 pose-replay seam (r60-research.md P1): citro3d's frame GX queue (a gxCmdQueue_s). After
 * C3D_FrameEnd it still holds every entry of the frame, in submission order, kicked chunks
 * included (a kick only STARTS the queue, it never clears it), until the next C3D_FrameBegin's
 * wait-and-clear. The command buffer and the backend's VBO pool are likewise untouched until the
 * next fresh StartFrame, so a replay can re-submit the frame without a full render. */
const void* gdx3ds_overlap_frame_queue(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_OVERLAP_H */
