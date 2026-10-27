/* port/3ds/gdx3ds_widescreen.h -- `[display] widescreen` (default 0): true widescreen races.
 *
 * WHAT IT IS. The 3DS top screen is 400x240 (5:3). Every frame already renders hor+: the
 * renderers' AdjXForAspectRatio compresses clip x by (4/3)/(5/3) = 0.8 (the G-Diffuser PC port's
 * `gEnhancements.Graphics.Widescreen`, on by default on both platforms), so 3D geometry outside
 * the game's 4:3 frustum lands in the side bands. What stopped that from being a real widescreen
 * picture on the 3DS was everything AROUND the projection:
 *   - the CRT-safe frame (viewport 296x224 + scissor 12,8..308,232 in N64 pixels: a 370x208
 *     window on the 3DS) -- which `[display] border_mode` then rescales to fill the screen,
 *     changing the aspect ratio (FULL-BLEED) or cropping (ZOOM/HYBRID);
 *   - the game's CPU-side culls (course chunks, racers, stars), which test the 4:3 NDC band,
 *     so edge content pops in;
 *   - the 2D HUD, compressed into the centre 4:3 band.
 *
 * The lever turns on, IN RACE MODES ONLY (GP, practice, time attack, VS 2-4P, death race), the
 * PC port's own switches for those three, which the shared decomp and both 3DS interpreters
 * (LUS carve + the owned one + NRR HUD) already implement:
 *   gEnhancements.Graphics.RemoveBorders  1P only: the full 320x240 frame (vertical FOV and
 *                                         framing stock; the picture fills 400x240 at 5:3)
 *   gEnhancements.Graphics.UltrawideMode  the culls widen by 1/0.8 = 1.25 (course chunks,
 *                                         racers, stars). The racer pass is made SIM-SAFE by
 *                                         decomp-3ds-widescreen-cull.patch: the extra machines
 *                                         are drawn but the race never sees them as visible.
 *   gEnhancements.Graphics.WidescreenUI   the HUD anchored to the screen edges (split screen
 *                                         too: WidescreenSplitUI defaults on)
 * and tells the backend (gfx_citro3d.cpp) to stop applying the border-mode scale to that
 * frame: 1P renders 1:1 (nothing left to fill), VS keeps the chosen border mode's SCENE scale
 * (each view keeps the N64 split frame) but never scales the UI, so the anchored HUD is not
 * pushed off screen. Menus, the ceremony and the editors are untouched: no CVar moves outside
 * a race, and border_mode applies there exactly as before.
 *
 * Lever off: nothing is published, no CVar is ever set, every renderer reads the same values it
 * always did -- the off build is byte-identical in output. Read once at the first publish after
 * the ini is loaded; changing it needs a restart.
 *
 * Credit: the game-side widescreen code (RemoveBorders / UltrawideMode / WidescreenUI and the
 * renderer math behind them) is the G-Diffuser PC port's (github.com/Zorkats/G-Diffuser, MIT),
 * shared by this tree's decomp pin f7fd0fd and the vendored libultraship carve. */
#ifndef GDX3DS_WIDESCREEN_H
#define GDX3DS_WIDESCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Frame kinds the game side publishes (port/input_bridge.c gdx_fixed_aspect_publish). */
enum {
    GDX3DS_WIDE_NONE = 0,  /* lever off, or not a race mode */
    GDX3DS_WIDE_1P = 1,    /* one full-screen view */
    GDX3DS_WIDE_SPLIT = 2, /* VS 2-4P: per-view hor+, split HUD anchors */
};

/* 1 when `[display] widescreen` is on (0 before the ini is loaded; latched after). */
int gdx3ds_widescreen_lever(void);

/* Game thread, once per tick (and at a mode flip): the kind of the frame being built. A no-op
 * while the lever is off. Sets the three CVars only when the kind changes. */
void gdx3ds_widescreen_publish(int kind);

/* The last published kind (any thread; a plain int read). The backend latches it per frame. */
int gdx3ds_widescreen_frame_kind(void);

/* `[display] widescreen_cull_pct` (default 115, clamped 100..200) as a factor: the extra slack
 * the 1P widescreen frame gives the game's CPU-side x culls on top of the hor+ 1.25.
 *
 * WHY. The course chunk cull tests each chunk's CENTRE against the NDC band. Stock, that band
 * (+-1.0) ends past the visible picture: the CRT-safe scissor (12..308 of 320) hides the outer
 * 160/148 = 1.08, so a chunk popping at the band edge popped behind the border. RemoveBorders
 * shows the full frame, so at exactly +-1.25 the pop happens ON the screen edge: a building or
 * wall whose centre leaves the band vanishes while its near half is still in the outer ~40 px,
 * and the sky shows through a side band (New 3DS report: DD-1/DD-2 courses, both edges). 3D
 * makes it worse: each eye's far-field parallax shifts the edge by a further ~6%. 1.15 covers
 * the lost border plus the stereo shift. Render-only: the chunk draw states feed nothing but
 * Course_Draw / Course_GadgetsDraw, the racer pass is sim-safe at any width
 * (decomp-3ds-widescreen-cull.patch). Read by the LUS interpreter's
 * gdx_get_ultrawide_cull_xscale() on 1P widescreen frames only (RemoveBorders on); split
 * frames keep their borders and the plain 1.25. 1.0 before the ini is loaded. */
#define GDX3DS_WIDE_CULL_PCT_DEFAULT 115
float gdx3ds_widescreen_cull_margin(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_WIDESCREEN_H */
