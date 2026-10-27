/* port/3ds/gdx3ds_pipe.h -- o3ds60: the Old 3DS 60 fps pipeline levers and their in-race A/B.
 *
 * hw a3b4531 (Old 3DS guest, 2-machine race, 3D on): every 60 fps stint ran rw 12.5-14.5 ms +
 * post 2.1-4.5 ms and fell back to 30 fps. Two structural costs sat on that path:
 *
 *   - the pace end guard (gfx/gdx3ds_pace.c): with two top framebuffers a frame's display
 *     transfer must wait until the previous frame's swap has latched (a VBlank0 after it), or it
 *     would write into the buffer on screen. A frame that finishes 0.5 ms after a vblank whose
 *     previous swap came after that vblank waits for the NEXT one: up to a whole period in
 *     EndFrame (fe= in [steadyd1], 0.5-2.2 ms average, max 3.9).
 *     `[debug] top_triple` (default 1 on an Old 3DS = in lockstep races, 2 = always, 0 = off; 0
 *     on a New 3DS): a third top framebuffer (576 KB, both eyes, the top screen's own memory)
 *     joins the rotation (vendor/libctru_gfx/gfx_split.c), the back buffer is always the one
 *     neither on screen nor pending, and the end guard does not wait while it runs. One frame
 *     of display latency at most, as a late double-buffered frame already had.
 *   - the bottom dash ran after EndFrame (post). `[ui] dash_pre_frame` (default 1 on an Old 3DS,
 *     0 on a New 3DS; lockstep races only): a rendering iteration runs the bottom-screen tick
 *     (gdx3ds_menu_tick: the race dash, its present) right before StartFrame, where the CPU
 *     would otherwise wait for the GPU to finish the previous frame (gsp= in [steadyd1]).
 *
 * `[debug] pipe_ab` = N frames (default 0 = the levers as configured): alternate blocks of N
 * (rounded to the 256-frame [frameskip] receipt window) inside every lockstep race, block 0 with
 * the levers as configured ("new"), block 1 with both off ("old"), so one hardware launch
 * measures both. Receipts: [pipe] init, [pipe] per 256-frame window in a lockstep race (block,
 * state, the triple-buffer counters), pipe= in [steadyd1], tb= in [pace]. Timing only: the game
 * state and every rendered frame are the same. */
#ifndef GDX3DS_PIPE_H
#define GDX3DS_PIPE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The frameskip's lockstep edge (gdx3ds_frameskip_set_lockstep). */
void gdx3ds_pipe_set_lockstep(int on);
/* Every iteration, right after the frameskip decision: asks the gfx layer for the top-screen
 * rotation the current block wants (applied at the next swap). Main thread. */
void gdx3ds_pipe_iteration_begin(void);
/* 1 when this rendering iteration runs the bottom-screen tick before StartFrame. */
int gdx3ds_pipe_dash_pre_frame(void);
/* A [frameskip] receipt window closed (every 256 iterations): the A/B block clock + [pipe]. */
void gdx3ds_pipe_window(unsigned long frame);
/* [debug] kick_3d (default 1): 1 = the [overlap] mid-frame GPU kicks (and the stereo head run) in
 * 3D frames as in 2D; 0 = none while the right eye runs. hw 9088d3a (3D, D = 1): fe= (EndFrame)
 * 4.7 ms avg, 8.2 max with ~2 kicks a frame, 0.8 ms in 2D: the frame end drains the kicked queue
 * (both eyes' kicked chunks) on the CPU before C3D_FrameEnd. With `[debug] pipe_ab_what = 2` the
 * pipe_ab odd blocks flip kick_3d instead of top_triple / dash_pre_frame. [steadyd1] drain=. */
int gdx3ds_pipe_kick_3d(void);
/* For receipts: -1 = the levers are not engaged (no lockstep race and top_triple != 2), 0 = an
 * "old" A/B block, 1 = the levers as configured. */
int gdx3ds_pipe_receipt_state(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_PIPE_H */
