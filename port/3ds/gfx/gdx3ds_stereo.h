/* port/3ds/gfx/gdx3ds_stereo.h — stream S: stereoscopic-3D foundation for the
 * citro3d backend (docs/research/stereo3d-research.md).
 *
 * This is PLUMBING, not the finished stereo feature: dual render targets, the
 * per-eye draw loop's injection point, the slider poll, and the per-drawcall
 * stereo-class channel — everything the later "stereo shift" needs so that shift
 * is only projection math + tuning (see port/3ds/gfx/STEREO.md for the
 * remaining-work list).
 *
 * Design constraints (mission + research):
 *  - Default OFF via config ([stereo] enabled, gdx3ds_config — read-only use);
 *    when off the backend behaves bit-identically to a build without this TU.
 *  - Slider-driven per frame (osGet3DSliderState, 0.0-1.0, official linear
 *    pattern); the right-eye pass is skipped entirely at slider == 0.
 *  - Per-eye pass reuses the already-built VBO — only the target binding and the
 *    projection uniform change for the second eye (NO CPU vertex re-copy; the
 *    sm64-3ds lineage re-copied per eye, refuted-as-necessary by the research).
 *  - The eye transform is an abstracted injection point (ComputeEyeMatrix):
 *    off-axis (asymmetric-frustum) stereo in clip space, mathematically exact
 *    for the game's perspective projections without knowing their parameters
 *    (derivation in the .cpp). Slider-scaled live; [stereo] iod / convergence
 *    tune strength and screen plane.
 *
 * Ownership: stream S. Hooks in gfx_citro3d.cpp are deliberately minimal.
 */
#pragma once

#include <cstdint>

#include <3ds.h>
#include <citro3d.h>

/* Per-drawcall stereo class (the s2DMode precedent from sm64-3ds, generalized).
 * Foundation behaviour:
 *   SCENE            — full eye offset (placeholder shear for now).
 *   UI_ZERO_PARALLAX — zero eye offset: the draw lands at screen depth in both
 *                      eyes (HUD/menus). Heuristic default for ortho draws.
 *   SKY_DEEP         — backgrounds that must sit behind all 3D geometry
 *                      (research: SM64 skybox artifact class): pinned to
 *                      far-plane parallax regardless of drawn depth. Bridge
 *                      tagging of the background draws is still pending.
 */
enum Gdx3dsStereoClass {
    GDX3DS_STEREO_SCENE = 0,
    GDX3DS_STEREO_UI_ZERO_PARALLAX = 1,
    GDX3DS_STEREO_SKY_DEEP = 2,
    GDX3DS_STEREO_ANCHORED = 3, /* ortho draw pinned to a world depth (prim-depth rects: race markers) */
    /* mstereo [stereo] menu_depth: a screen-space draw of a MENU frame, at the layer depth its
     * coverage gives it (SetMenuUiDraw). Internal: never set through gdx3ds_stereo_set_draw_class. */
    GDX3DS_STEREO_MENU_UI = 4,
};

#ifdef __cplusplus
extern "C" {
#endif

/* Bridge-layer tag channel (later work wires actual game call sites): sticky
 * override for subsequent draws, GDX3DS_STEREO_* or -1 to return to the
 * heuristic default (ortho draw -> UI_ZERO_PARALLAX, else SCENE). Safe to call
 * from any layer; a no-op cost when stereo is off. */
void gdx3ds_stereo_set_draw_class(int stereoClass);

/* Force-enable regardless of config — for the DL harness, which does not link
 * the OS/config library. Must be called before the backend's Init(). */
void gdx3ds_stereo_request_enable(void);

/* o3ds60 the Home screen's 3D button: [stereo] enabled as this session runs it (1 = the slider
 * decides), and the button's flip. set_user_on writes [stereo] enabled into the config table (the
 * caller saves the ini) and takes effect on the next frame; 1 in a session that booted with 3D
 * off creates the right-eye target then. In an online race [stereo] mp_ab / mp_3d decide first
 * (mp_3d = 0, the Old 3DS default: 2D unless the button was pressed in this launch). */
int gdx3ds_stereo_user_on(void);
void gdx3ds_stereo_set_user_on(int on);

/* B11q. Mark the OPEN frame as a TASKLESS HOLD PRESENT: a frame that puts no draws on
 * the screen target and instead restores it from the frame mirror with a framebuffer
 * COPY (n64_gfx_bridge.cpp, gdx_vi_present_fallback -> Interpreter::CopyFrameBuffer).
 *
 * A copy has no right-eye counterpart -- GfxRenderingAPIC3D::CopyFramebuffer writes
 * mFramebuffers[dst] and nothing else -- while the clear that precedes it DOES mirror
 * onto the right eye (ClearRight). The right target was therefore cleared to black and
 * then never refilled, and was still transferred to GFX_RIGHT at C3D_FrameEnd: with the
 * 3D slider up, every hold present blanked one eye.
 *
 * Marking the frame makes ClearRight a no-op for it, so the right target keeps the
 * content it already holds -- which is this very game frame, drawn with its own eye
 * matrix, since the mirror the left eye is restored from was captured from the same
 * frame. Killswitch [debug] stereo_hold_keep (default 1). Cleared by the backend at
 * EndFrame, so the mark never outlives one present. */
void gdx3ds_stereo_mark_hold_frame(void);

/* Verification readback of the RIGHT eye's color buffer (same output contract
 * as GfxRenderingAPIC3D::ReadFramebufferToCPU: packed RGBA5551, row 0 = scene
 * top, coverage bit forced). Call OUTSIDE an active C3D frame (after EndFrame).
 * Returns 0 on success, nonzero when stereo is inactive/unavailable. */
int gdx3ds_stereo_read_right(uint16_t* rgba16Buf, uint32_t width, uint32_t height);

/* mstereo [stereo] menu_depth (default 0): the menu screens' depth layout. With the lever on and the
 * right-eye pass active, a frame whose game mode is a menu (title, main menu, machine select /
 * settings, course select, the GP next-course card, records course select, options) places
 *   - its screen-space draws by the size of their rectangles (the largest width or height of any one
 *     triangle): a backdrop (>= 272 px: the art drawn in full-width strips) `menu_back` px behind
 *     the screen, a panel / icon / portrait / banner (32 .. 160 px) at the screen plane, text glyphs, arrows and
 *     cursors (<= 24 px) `menu_front` px in front, linear ramps between (an animated element that
 *     grows slides, it never pops); a draw that touches the left or right screen edge stays at the
 *     plane (a shifted edge would uncover a strip in one eye);
 *   - its perspective (SCENE) draws -- the menu's 3D models, the tiled transitions' captured screen
 *     -- flat at `menu_model` px (default 0: on the plane with the panels and icons they are drawn
 *     over and under, in front of the backdrop), instead of the race convergence's far-plane
 *     parallax (+2 x iod: the menu cameras put every model at NDC depth 0.96 .. 0.999).
 * Every value is a disparity (right eye minus left eye) in top-screen pixels at full slider and the
 * default iod (12), scaled by slider x iod / 12: the 3D slider and [stereo] iod act on menus as they
 * do on races. Slider 0 / stereo off: nothing here runs (byte-identical mono). Non-menu frames
 * (races, records flyby, demos, credits, editors): unchanged. */
/* 1 while the open frame takes the menu depth layout (latched at the frame's StartFrame). */
int gdx3ds_stereo_menu_frame(void);
/* 1 with the menu_depth lever on: the machine settings' flat backdrop fills the frame (hwfix-combined;
 * decomp object.c declares it locally). */
int gdx3ds_stereo_menu_backdrop_full(void);
/* The NRR's per-draw row of a screen-space menu draw (`verts`: packed triangles, `stride` floats per
 * vertex, clip x y z w first): the MENU_UI row of its coverage. */
void gdx3ds_stereo_menu_ui_row(const float* verts, uint32_t nVerts, uint32_t stride, float row[4]);
/* The end vertex of the run of a menu screen-space draw that starts at vertex `first` (nVerts when
 * the rest is one layer; Gdx3dsStereo::MenuUiRunEnd), for the NRR menu runner's batches. */
uint32_t gdx3ds_stereo_menu_run_end(const float* verts, uint32_t nVerts, uint32_t stride, uint32_t first);

#ifdef __cplusplus
} /* extern "C" */

namespace Gdx3dsStereo {

/* Read config, and when enabled: create the 240x400 right-eye color+depth
 * target bound to GFX_TOP/GFX_RIGHT and switch the LCD to stereo scanout
 * (gfxSet3D(true) — mutually exclusive with 800px wide mode). Called from the
 * backend's Init() after the main (left) target exists. Returns the enabled
 * state; on any allocation failure stereo stays off and mono is unaffected. */
bool Init();

bool Enabled();

/* o3ds60: the Home 3D button turned 3D on in a session that booted without it. The backend's
 * StartFrame then calls Init() (once). Also runs the online-race receipts while stereo is off. */
bool WantInit();

/* Per-frame gate, called from StartFrame() inside the C3D frame (only when
 * Enabled()): polls the slider, and when slider > 0 queues the right target
 * into the frame (C3D_FrameDrawOn — required for its display transfer at
 * C3D_FrameEnd) before re-binding `mainTarget`. Returns this frame's "active"
 * state: true iff the per-eye draw loop should run. */
bool FrameBegin(C3D_RenderTarget* mainTarget);

/* True iff FrameBegin() latched the right-eye pass on for this frame. */
bool Active();

C3D_RenderTarget* RightTarget();

/* B11q trace: true iff the right target is queued into the OPEN frame, i.e.
 * C3D_FrameEnd will run its display transfer (citro3d sets C3D_RenderTarget::used
 * in C3D_FrameDrawOn and clears it in C3D_FrameEnd). Read before C3D_FrameEnd. */
bool RightQueued();

/* Raw mid-frame target switch: C3D_SetFrameBuf without C3D_FrameDrawOn's
 * viewport/scissor reset (research: the sm64-3ds frame_draw_on precedent), so
 * the interpreter's current viewport/scissor state applies to both eyes. */
void BindTargetRaw(C3D_RenderTarget* target);

/* Mirror a main-target clear onto the right eye (only when Active()). */
void ClearRight(C3D_ClearBits flags, uint32_t color, uint32_t depth);

/* B11q: end-of-present hook. Clears the taskless-hold mark (see
 * gdx3ds_stereo_mark_hold_frame) and returns the cumulative number of hold presents
 * whose right eye was preserved, for the [c3d] window receipt. */
unsigned long EndFrameHold();

/* THE eye-transform injection point. eye: -1 = left, +1 = right, 0 = center
 * (mono / zero parallax). Writes `base` patched for the eye into `out`.
 *
 * Implementation: off-axis (asymmetric-frustum) stereo applied in CLIP SPACE —
 * x' = x ± k·(z − dc·w), k = sep·slider/(1−dc) — folded into the base matrix's
 * x-coefficients. Mathematically exact (see the derivation in the .cpp): a
 * shift affine in NDC depth is the same family Mtx_PerspStereoTilt generates,
 * applied post-hoc because the game owns the projection and vertices arrive
 * CPU-pre-transformed. Tuning: [stereo] iod (far-plane parallax in pixels at
 * full slider, default 12) and [stereo] convergence (NDC depth of the
 * zero-parallax plane, default 0.25), scaled live by the 3D slider.
 *
 * stereoClass semantics: UI_ZERO_PARALLAX returns the center matrix (screen
 * depth, no doubling of HUD/2D); SKY_DEEP pins the draw to far-plane parallax
 * (behind all scene geometry); SCENE applies the depth-dependent shift. */
void ComputeEyeMatrix(const C3D_Mtx* base, int eye, C3D_Mtx* out,
                      int stereoClass = GDX3DS_STEREO_SCENE);

/* Per-draw stereo class: the bridge override when set, else the heuristic
 * (`orthoDraw` — the backend passes clip-w == 1, i.e. an orthographic
 * projection, the research's HUD signature). */
int ClassifyDraw(bool orthoDraw);
/* [anchor] NDC depth (0 near .. 1 far, the interpreter z/w convention) for the next
   GDX3DS_STEREO_ANCHORED draw: the N64 prim depth of a marker rect, i.e. the machine it labels. */
void SetAnchorDepth(float d);

/* B11q: true while the open frame is a taskless hold present (no right-eye content to make). */
bool HoldFrame();

/* mstereo [stereo] menu_depth (see gdx3ds_stereo_menu_frame above). MenuFrame: the open frame takes
 * the menu layout. MenuUiClass: the class of a screen-space draw of such a frame (its triangles:
 * `verts`, `stride` floats per vertex, clip x y z w first) -- GDX3DS_STEREO_MENU_UI with its layer
 * shift latched for the draw that follows, or UI_ZERO_PARALLAX when its layer is the screen plane. */
bool MenuFrame();
int MenuUiClass(const float* verts, uint32_t nVerts, uint32_t stride);
/* The end vertex of the run of a menu screen-space draw starting at vertex `first` (nVerts when the
 * draw is one layer): a batch is cut where its rectangles change layer class, nine-slice pieces kept
 * together (MenuUiRunEndImpl). The per-draw loop, the frame replay and the NRR menu runner. */
uint32_t MenuUiRunEnd(const float* verts, uint32_t nVerts, uint32_t stride, uint32_t first);
/* The [stereo] menu receipt's model= count: a SCENE draw of a menu frame (the backend calls it). */
void MenuNoteModel();

/* ---- n60stereo: the stereo FRAME REPLAY (gdx3ds_stereo_frame.cpp, STEREO.md "Frame replay") ----
 *
 * The per-draw loop above switches targets twice and uploads two projections per draw. The frame
 * replay instead emits every draw ONCE, eye-independent, and makes the right eye by re-submitting
 * the same command lists:
 *   - the shader skews the incoming clip x by `steye * dot(stk, clip)` (shader.v.pica). `stk` is
 *     a per-draw, eye-INDEPENDENT row (the draw's stereo class: ClassRow); `steye` is -1 / +1 /
 *     0 and is written raw, only OUTSIDE the replayed body (never through citro3d);
 *   - ReplayEnter (the first NRR hand-off of a stereo-active frame) closes the command list built
 *     so far (those draws took the per-draw loop: both eyes are drawn already), writes steye = -1
 *     and the main target's framebuffer words, and makes the rest of the frame self-contained
 *     (every citro3d state group and uniform re-emitted at the first draw);
 *   - ReplayFrameEnd (EndFrame, after the [overlap] drain, before C3D_FrameEnd) closes the body,
 *     adds a prologue (the right target's framebuffer words, steye = +1), re-adds the body's GX
 *     queue entries (command lists as they are; main-target clears retargeted to the right eye;
 *     mid-frame captures / copies / uploads skipped: they are mono, as in the per-draw loop) and an
 *     epilogue (steye = 0). C3D_FrameEnd then queues both display transfers after them.
 * The CPU cost of the right eye is that prologue/epilogue and a few queue entries; the GPU runs
 * the body twice. Killswitch [native] stereo_replay (default 1); 0 = the per-draw loop for every
 * draw, and the NRR categories refuse under stereo (the pre-n60stereo behaviour). */

/* Backend Init (after Init()): the shader's `stk` / `steye` registers, the main target. Latches
 * the lever and prints `[stereo] replay ...`. stkLoc / eyeLoc < 0 = the shader has none: off. */
void ReplayInit(int stkLoc, int eyeLoc, C3D_RenderTarget* mainTarget);
/* StartFrame, every frame (also mono): the per-frame reset, and steye = 0 raw when the GPU may
 * have lost it (boot, a HOME / sleep return). Inside the C3D frame. */
void ReplayFrameBegin();
/* The NRR's "may I draw" (gdx3ds_nrr_bk_frame_active): true when the frame is (now) in replay
 * mode. False: stereo inactive / lever off / a hold frame / the GX queue cannot hold the replay. */
bool ReplayEnter();
/* True between ReplayEnter and ReplayFrameEnd. */
bool ReplayActive();
/* niview: read-only snapshot of the latest outcome ReplayEnter() decided (sticky across frame
 * boundaries -- NOT reset at ReplayFrameBegin; see the sReplayOkThisFrame comment in
 * gdx3ds_stereo_frame.cpp for why a strict per-frame clear broke the tick-ahead pipe). Never calls
 * ReplayEnter() itself. A game-side reader (a category's admission check, ahead of the render thread
 * in the pipe) may see the previous render frame's outcome, not this exact one -- acceptable, same
 * staleness as this codebase's other cross-thread signals. See gdx3ds_stereo_nrr_ok() in
 * gdx3ds_stereo_frame.cpp for why this indirection exists at all -- calling ReplayEnter() itself from
 * the wrong thread crashed (GPUCMD_Add, svcBreak), a real gated finding. */
bool ReplayOkThisFrame();
/* StartDrawToFramebuffer(fbId) inside the body. A texture target: noted. The main target after a
 * texture pass: the main framebuffer words go into their own command list, which the right eye's
 * replay swaps for the right target's. The main target with no texture pass since the last bind:
 * the GPU already holds this eye's target, so no framebuffer words at all. */
void ReplayNoteBind(int fbId);
/* EndFrame, after gdx3ds_overlap_before_frame_end, before C3D_FrameEnd. */
void ReplayFrameEnd();
/* [overlap] kick (after its split + queue run): with [native] stereo_interleave (default 1) the chunk
 * the kick just submitted is replayed for the right eye right behind it, then the left eye resumes
 * with a fresh self-contained start. The GPU draws both eyes of the early categories while the CPU
 * builds the late ones; the frame end replays only the last chunk. */
void ReplayKickChunk();
/* n60tail [native] stereo_fold: the [overlap] kick, right before its split. When the kick chunk will
 * replay, the right prologue is written at the end of the list the split closes (no prologue list). */
void ReplayKickPreSplit();
/* The stk row of a stereo class in replay mode (eye sign NOT applied: the shader multiplies by
 * steye). UI_ZERO_PARALLAX / unknown: 0. ANCHORED uses SetAnchorDepth's depth. */
void ClassRow(int stereoClass, float row[4]);
/* The citro3d uniform location of stk (-1 none): DrawTriangles' upload. */
int ReplayStkLoc();
/* n60tail [native] sky_late: the next command list starts self-contained (citro3d's whole state and
 * every uniform re-emitted before its first draw); the framebuffer binding is left as it is. */
void MarkStateSelfContained();
/* n60st3 ([debug] spike_ms): the right-eye CPU (enter + kick chunks + frame end) of the latest
 * frame since its StartFrame; *chunks = its kick chunks. */
uint64_t FrameCpuTicks(unsigned* chunks);

} // namespace Gdx3dsStereo

extern "C" {
#endif /* __cplusplus */

/* n60stereo, C side: 1 when an NRR category may consume / draw under the current [stereo] config
 * (stereo off, or [native] stereo_replay on). The boot-time category refusals use it. */
int gdx3ds_stereo_nrr_ok(void);
/* 1 while the open frame is in frame-replay mode: NRR draws then upload their class's stk row. */
extern int gdx3ds_stereo_replay_frame;
/* The stk rows of this frame: [0] SCENE, [1] UI (zero). Valid while gdx3ds_stereo_replay_frame. */
extern float gdx3ds_stereo_rows[2][4];
/* Nonzero while an NRR hand-off owns citro3d (NrrBegin .. NrrEnd): no chunk boundary inside it. */
extern int gdx3ds_nrr_handoff_open;
/* C entry of Gdx3dsStereo::ReplayKickChunk (gdx3ds_overlap.c). */
void gdx3ds_stereo_kick_chunk(void);
/* C entry of Gdx3dsStereo::ReplayKickPreSplit (gdx3ds_overlap.c, n60tail). */
void gdx3ds_stereo_kick_pre_split(void);
/* n60tail: bit 0 = stereo_fold, bit 1 = stereo_xl_early in force for the frame being built. */
int gdx3ds_stereo_tail_phase(void);
/* 1 while the right-eye pass is active (Gdx3dsStereo::Active, latched at StartFrame): the pace rule. */
int gdx3ds_stereo_active(void);
/* C entry of Gdx3dsStereo::FrameCpuTicks (the spike receipt). */
unsigned long long gdx3ds_stereo_frame_cpu_ticks(unsigned* chunks);

#ifdef __cplusplus
} /* extern "C" */
#endif
