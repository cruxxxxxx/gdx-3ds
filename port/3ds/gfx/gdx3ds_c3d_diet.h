/* port/3ds/gfx/gdx3ds_c3d_diet.h -- c5submit (r60 P3a): the citro3d submission diet's levers.
 *
 * The diet itself lives in the vendored citro3d (port/3ds/vendor/citro3d/source/gdx_submit.c,
 * control surface gdx/c3d_gdx.h). This module latches the ini once, sets citro3d's mode word,
 * and prints the receipts. docs/research/native/c5submit-progress.md.
 *
 *   [native] c3d_diet        0 (default) stock citro3d; 1 the diet (byte-identical command stream)
 *   [native] c3d_diet_mask   which diet parts (default 7): 1 word-wise uniform scan (VS + GS),
 *                            2 inline command emission, 4 skip the context update when clean
 *                            (1|2|4 = byte-identical); 8 = ELIDE, drop VS float-uniform registers
 *                            the GPU already holds (fewer words, same GPU state; needs 1; NOT
 *                            byte-identical, NOT safe with anything that replays/patches command
 *                            buffers unless it calls C3Dgdx_InvalidateUniformShadow)
 *   [native] c3d_diet_verify 1: every draw runs stock AND diet from the same citro3d state and
 *                            compares their command words and resulting state (stock is kept;
 *                            with ELIDE the compare is semantic and the diet is kept);
 *                            2: the same plus a self-test that corrupts every 997th diet draw
 *   [debug]  c3dprof         1: per-section split of the stock path; 2: whole draw call timed
 *                            (stock, or the diet when c3d_diet=1). Profiles print with gputrace
 *                            or on their own.
 *   [debug]  c3dprof_every   N (default 1): profile one 64-frame window of every N (onesubmit: the
 *                            cheap sampled mode for a hardware card)
 *
 * Receipts: `[c3ddiet] init diet=.. mask=.. verify=.. prof=.. vendored=..` once, and with verify
 * or a profile armed, `[c3ddiet] frame=` / `[c3dprof] frame=` every 64 frames. */
#ifndef GDX3DS_C3D_DIET_H
#define GDX3DS_C3D_DIET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Per fresh frame, before any draw (latches the ini once it is loaded). */
void gdx3ds_c3d_diet_frame_begin(void);
/* Per frame after C3D_FrameEnd (window counters and receipts). */
void gdx3ds_c3d_diet_frame_end(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_C3D_DIET_H */
