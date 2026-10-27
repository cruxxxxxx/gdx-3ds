/* port/3ds/gfx/nrr/gdx3ds_nrr.h -- the Compact Native Race Renderer (NRR) core.
 *
 * Design: docs/research/native/n60-design.md (sections 2.2, 2.3, 5, 7, 1.1). Progress, gotchas:
 * docs/research/native/n60core-progress.md. Every category is lever-gated, default OFF; with every
 * lever off the core is one int test at boot and nothing else runs.
 *
 * ================================================================== HOW TO ADD A CATEGORY ====
 *
 *  1. Pick its id (NRR_CAT_*) below. Write ONE game-side file and ONE render-side file in
 *     port/3ds/gfx/nrr/ (e.g. gdx3ds_nrr_sky_game.c + gdx3ds_nrr_sky.cpp) and add both to the
 *     gdx3ds_n64 OBJECT library in port/3ds/gfx/CMakeLists.txt (objects, not an archive: the core
 *     finds the category through a WEAK reference, which never extracts an archive member).
 *  2. Define the category's init with exactly this name (the core calls it once, at boot, only
 *     when gdx3ds_nrr_level(cat) != 0):
 *         void gdx3ds_nrr_cat_init_<name>(void);   name = hud trk mach sky dress shd fx selftest deco
 *     In it: gdx3ds_nrr_set_draw(cat, YourDraw) and, for a pure c5tick section,
 *     gdx3ds_nrr_register(cat, NRR_SEC(SKY), YourSnap). A category that is not a consumed section
 *     (the HUD: its emit sites carry game logic; the track: a decomp patch returns after the cull)
 *     calls gdx3ds_nrr_frame_build() + NrrSnap* + gdx3ds_nrr_write_marker() from its own hook.
 *  3. Snapshot (game fiber, inside the build of tick N): copy the few fields the draw needs into
 *     the frame: `MySnap* s = NrrSnapAlloc(f, sizeof *s); ...; NrrSnapSet(f, cat, view, s);`.
 *     Never keep pointers to live game state in a snapshot (see THE LOOK-AHEAD below).
 *  4. Draw (render side, at the 0x63 marker): `int YourDraw(NrrCtx* c, int view, const NrrFrame* f)`.
 *     The core has already called NrrBegin (the interpreter's open batch is drawn, painter's order)
 *     and calls NrrEnd after you return. Inside: NrrStateGet (cold, cache the pointer) ->
 *     NrrStateApply -> NrrSetInputs (per-draw prim/env) -> NrrRingAlloc -> write vertices ->
 *     NrrDraw2D / NrrDrawXf. Return 0 = drawn, nonzero = "I could not": the core counts a
 *     fallback (`[nrr] fallback`) -- there is no legacy path left for a consumed section, so a
 *     steady race must report 0.
 *  5. Hot code: name every function the steady loop runs `Nrr<Cat>Hot*` (or list it in
 *     tools/native/nrr_hot.list) so tools/native/nrr_size.sh counts it against design 1.1.
 *     Cold paths (key build, first-sight imports, receipts) go GDX_COLD / noinline, never inline.
 *
 * ========================================================================= THE LOOK-AHEAD ====
 *
 * The list built in host iteration N is walked in iteration N+1 (c5tick-progress 3.3). Verified
 * by reading port/3ds/main_3ds.cpp + port/n64_sched.c + port/n64_vi.c, renderthread=0:
 * func_80067E98 / func_80067D64 build list N, then spin on osViGetCurrentFramebuffer, which
 * YIELDS to the host (n64_vi.c) -- iteration N ends there. Iteration N+1's gdx_vi_tick posts the
 * retrace; the resumed game fiber leaves the spin and calls Gfx_SetTask(list N) ->
 * osSpTaskStartGo -> gdx_gfx_run SYNCHRONOUSLY (the walk, our 0x63 markers), and only after it
 * returns does the frame function loop to osRecvMesg(VI) and run tick N+1's logic
 * (func_800690FC) and build N+1. So with renderthread=0 tick N+1's logic does NOT run before list
 * N is walked. What does run between build N and walk N: Audio_Update (67E98 order), the DP wait,
 * Transition_SetBackgroundBuffer, Segment_LoadAssets, the host's input poll and the audio thread.
 * Snapshots exist anyway, because:
 *   - renderthread=1 (New 3DS pipe mode): walk N runs on core 2 CONCURRENTLY with tick N+1's logic;
 *   - the work between build N and walk N above (segment loads move asset pointers);
 *   - a pure function of the snapshot is what a core-1 render-prep (design 8) needs.
 * The receipt's `lag=` field measures it: gGameFrameCount at draw minus at snapshot (0 expected
 * on renderthread=0).
 *
 * ============================================================================== THREADING ====
 *
 * Game side (snapshots, marker writes, register): the game fiber. Render side (draw, state, ring):
 * whoever walks the list -- main on renderthread=0, the only mode the levers accept for now.
 */
#ifndef GDX3DS_NRR_H
#define GDX3DS_NRR_H

#include <stddef.h>
#include <stdint.h>

#include "../gdx3ds_dlb_shim.h" /* GDX_DLB_SEC_* for NRR_SEC */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------- ids ---- */

/* G_NOOP marker: w0 = (G_NOOP<<24) | (0x63<<16) | (view<<8) | cat, w1 = NrrFrame*, w1hi = the
 * decomp's host-pointer tag. 0x63 was free: 0x5A-0x62, 0x65, 0x6C-0x6E are taken (track, fx,
 * hud/shadow capture, track_retained, c5cat, c5sub, sky, dress scope, gadget runs); the stock LUS
 * G_NOOP handler ignores it. Dispatched by OwnedNoopHook (n64/gdx_rt_glue.cpp). */
#define GDX3DS_NRR_MARKER_P 0x63u

enum {
    NRR_CAT_HUD = 0,
    NRR_CAT_TRK = 1,
    NRR_CAT_MACH = 2,
    NRR_CAT_SKY = 3,
    NRR_CAT_DRESS = 4,
    NRR_CAT_SHD = 5,
    NRR_CAT_FX = 6,
    NRR_CAT_SELFTEST = 7, /* [native] nrr_selftest: debug only, never in a card */
    NRR_CAT_DECO = 8,     /* [native] nrr_deco (n60deco): the course decorations, rides nrr_dress */
    NRR_CAT_COUNT = 9
};

#define NRR_MAX_VIEWS 4

/* c5tick section bit for gdx3ds_nrr_register (ids: gfx/gdx3ds_dlb_shim.h, GDX_DLB_SEC_*). */
#define NRR_SEC(name) (1u << GDX_DLB_SEC_##name)

/* ------------------------------------------------------------------------- levers ---- */

/* [native] nrr (master) and nrr_hud nrr_trk nrr_mach nrr_sky nrr_dress nrr_shd nrr_fx (0/1, default
 * 0, NOT under all=1), [native] nrr_selftest (debug). Latched once, after
 * the ini is loaded. Refused (receipt `[nrr] REFUSED why=...`, everything reads 0) under
 * renderthread != 0, interp = 0, interp_verify != 0, [native] replay != 0 (C6 re-submits tagged
 * draws; NRR draws are untagged -- design 6 runs replay=0). Returns the category's effective level. */
int gdx3ds_nrr_level(int cat);
/* [native] ni_view (niview, default 0): bit (1 << cat) = category `cat` also draws the views of a
 * multi-view race (VS 2-4P, the 2P/4P attract demos); a clear bit keeps the category's players == 1
 * refusal. 1 only when the category's own level is on. (Bit 9, NRR_NI_VIEW_STEREO, used to also gate
 * this on no-stereo-frame-active; dropped -- nimenu W3, review finding M3: every caller of this
 * function only reaches it when gNumPlayers > 1, and Gdx3dsStereo::ReplayEnter already refuses the
 * frame replay unconditionally whenever gNumPlayers > 1 [gdx3ds_stereo_frame.cpp], so the per-eye
 * outer-edge mismatch this bit routed around could never actually occur at any of this function's
 * real call sites -- it was dead protective code against an unreachable case.) */
int gdx3ds_nrr_ni_view(int cat);
/* ni_view bit 10: the records flyby (GAMEMODE_RECORDS: Records_Draw's sky, course and dressing, one view,
 * the game's own camera) on the NRR -- sky, trk, dress (+ deco) with their own levels on. */
#define NRR_NI_VIEW_RECORDS 0x400
/* 1 when a non-race 3D scene may open its scope (c5tick gdx_dl_scene): the master on, the records bit,
 * one view. */
int gdx3ds_nrr_scene_ok(void);
/* ni_view bit 11 (native-vs): [native] ni_race's span programs also run in the frames of a 2-4P race
 * (VS, the 2P/4P attract demos), whose walk between the markers is otherwise the stock walker
 * (NiRaceScope, gdx_rt_glue.cpp, refuses VS 2P-4P without it). Needs ni_race on; independent of the
 * category bits (a span is the root words between whatever markers the frame has). */
#define NRR_NI_VIEW_RACE 0x800
/* o3ds60 [native] ni_fullview (default 0; the Old 3DS preset 0x97A = trk sky dress shd fx deco +
 * the race spans): ni_view's bits that also apply while the race is drawn MP fullview (one local
 * view of a 2-4P lockstep race, full screen: game/gdx3ds_fullview_game.c), so that view takes the
 * NRR the 1P race takes instead of the interpreter. Read: the category bits (NRR_NI_FULLVIEW_CATS,
 * game side, inside Race_Draw's fullview latch) and NRR_NI_VIEW_RACE (render side, with
 * [native] ni_race: the span programs in a frame whose race runs fullview; gdx_rt_glue.cpp). The
 * records bit stays ni_view's. Render only (the views' visibility pass and every simulation read
 * are unchanged). Receipt: `[nrr] init ... ni_fullview=0x...`, `[nirace] init ... fv=1`. */
#define NRR_NI_FULLVIEW_CATS 0x1FF
#define NRR_NI_FULLVIEW_BITS (NRR_NI_FULLVIEW_CATS | NRR_NI_VIEW_RACE)
/* 1 when the master is on and ni_fullview has NRR_NI_VIEW_RACE. */
int gdx3ds_nrr_ni_fullview_race(void);
/* 1 when the master is on and ni_view has NRR_NI_VIEW_RACE. */
int gdx3ds_nrr_ni_view_race(void);
/* A category's view scope: 1 for the 1P race's view 0, and for view `view` < `players` of a 2-4P race
 * when [native] ni_view has the category's bit; 0 otherwise. `players` = the game's gNumPlayers. */
int gdx3ds_nrr_view_ok(int cat, int view, int players);
/* niview: zeroed memory for a category's per-view buffers (views 1-3 of a multi-view race), allocated
 * once at the first multi-view build and kept (never freed). NULL when the heap is out. */
void* gdx3ds_nrr_view_alloc(uint32_t bytes);
/* niview receipt (render side, cold): a category's hand-off drew view `view` of a multi-view race. */
void gdx3ds_nrr_note_view(int view);
/* 1 when the master is on and was not refused. */
int gdx3ds_nrr_on(void);
/* Boot: latches the levers, prints `[nrr] init ...`, calls each enabled category's init.
 * Idempotent; called from the first c5tick latch (gdx3ds_c5tick.c). */
void gdx3ds_nrr_boot(void);

/* -------------------------------------------------------- game side: the snapshot ---- */

/* One per GfxPool, double-buffered exactly like the fx/track/HUD records (the game fills the
 * frame of pool P while the render side may still read pool P^1). Opaque outside the core;
 * categories reach their blocks through NrrSnapGet. */
typedef struct NrrFrame NrrFrame;

/* Bytes of snapshot arena per frame (all categories, all views). */
#define NRR_FRAME_ARENA_BYTES (12u * 1024u)

/* The frame of the GfxPool being built; reset on the first call of each build (keyed on
 * (gGfxPool, gGameFrameCount)). NULL with the master off. */
NrrFrame* gdx3ds_nrr_frame_build(void);
/* Bump allocation in the frame's arena, 8-byte aligned, zero-filled. NULL on overflow (counted
 * in the receipt as `ovf=`; the category must then not write its marker). */
void* NrrSnapAlloc(NrrFrame* f, uint32_t bytes);
/* The category's root block for `view` (one pointer per (cat, view)). */
void NrrSnapSet(NrrFrame* f, int cat, int view, const void* block);
const void* NrrSnapGet(const NrrFrame* f, int cat, int view);
/* gGameFrameCount at the frame's reset (the snapshot tick). */
uint32_t NrrFrameTick(const NrrFrame* f);

/* Write the 0x63 marker at `gfx` (the decomp's 16-byte port Gfx) and return gfx + 1 command. */
void* gdx3ds_nrr_write_marker(void* gfx, int cat, int view, NrrFrame* f);

/* A consumed section's snapshot: fill the frame for `view`; return 1 to write the marker, 0 to
 * write nothing (nothing to draw this view). */
typedef int (*NrrSnapFn)(NrrFrame* f, int cat, int view);
/* Consume every c5tick section in secMask (NRR_SEC bits) on rendered ticks; at each skipped
 * section the core runs snap() and writes the marker. Calls gdx3ds_c5tick_consume (which refuses,
 * with a receipt, a section that is not side-effect free). Returns the number of sections taken.
 * Only the lowest section id of the mask writes the marker (one draw per category and view). */
int gdx3ds_nrr_register(int cat, uint32_t secMask, NrrSnapFn snap);

/* --------------------------------------------------------- render side: contract ---- */

typedef struct NrrState NrrState;

/* Per-category counters of one receipt window (the core owns them; NrrDraw* fill draws/verts). */
typedef struct NrrCatStats {
    uint32_t markers;   /* 0x63 markers dispatched */
    uint32_t draws;     /* NrrDraw2D / NrrDrawXf calls */
    uint32_t states;    /* NrrStateApply calls that changed something */
    uint32_t verts;
    uint32_t fallbacks; /* draw functions that returned nonzero */
    uint32_t lagMax;    /* max (tick at draw - tick at snapshot) */
    uint64_t ticks;     /* svc ticks inside the category (only with the [c5cat] clock armed) */
} NrrCatStats;

typedef struct NrrCtx {
    int cat;
    int view;
    void* seam;                /* GdxInterpSeam* of the walking interpreter (NrrBegin drains it) */
    const NrrState* last;      /* last state block applied in this hand-off (NULL: none yet) */
    uint32_t stageConst[6];    /* TexEnv constant registers as NRR last wrote them */
    const void* lastVerts;     /* bound vertex buffer (pointer, stride) */
    uint32_t lastStride;
    uint32_t saved;            /* citro3d state groups saved before NRR changed them (NrrEnd restores) */
    int8_t lastAttr;           /* NRR_PROG_* attribute layout bound, -1 unknown */
    uint8_t projSet;           /* the projection uniform holds: 1 the 2D fixup, 2 the 3D scene map */
    uint8_t keepVp;            /* [n60mach] set before the first apply: draws keep the walk's viewport
                                  and scissor (a 3D category in a split-screen view: the view's, not
                                  the block's), and NrrDrawXf cuts the scissor to the game's clip
                                  square (the CPU clipper's edge); 0 = the full main target and
                                  the block's scissor */
    uint8_t scene2d;           /* set only inside NrrDraw2DScene: the packed draw takes the 3D scene map */
    uint8_t anchorSet;         /* n60stereo: anchorRow is the next 2D draw's stereo row (NrrStereoAnchor) */
    float anchorRow[4];
    NrrCatStats* st;
} NrrCtx;

typedef int (*NrrDrawFn)(NrrCtx* c, int view, const NrrFrame* f);
void gdx3ds_nrr_set_draw(int cat, NrrDrawFn fn);

/* n60trkv: a marker a category can take WITHOUT a hand-off (no NrrBegin / NrrEnd, no GPU word).
 * Called before the hand-off with the walking interpreter's seam; 1 = consumed (the category keeps
 * what it needs and draws it at a later marker of its own, in order), 0 = run the hand-off and the
 * category's draw as usual. A category holding consumed markers says so with
 * gdx3ds_nrr_set_pending(cat, 1): a marker of ANY other category then first runs a hand-off of the
 * pending one (its draw, with `view` = NRR_VIEW_DRAIN) so painter's order across categories holds. */
typedef int (*NrrPeekFn)(void* seam, int view, const NrrFrame* f);
void gdx3ds_nrr_set_peek(int cat, NrrPeekFn fn);
void gdx3ds_nrr_set_pending(int cat, int pending);
#define NRR_VIEW_DRAIN 0xFF

/* The marker's entry (OwnedNoopHook): validates the frame, NrrBegin, the category's draw,
 * NrrEnd, counters. `seam` = the walking interpreter's GdxInterpSeam. */
void gdx3ds_nrr_marker(void* seam, uint32_t w0, uint32_t w1);
/* Same without a marker (a category's own hook): draw `cat` for `view` from frame `f`. */
void gdx3ds_nrr_draw(void* seam, int cat, int view, const NrrFrame* f);
/* A category whose words arrive through its own hook with no snapshot frame (the HUD: its 0x5E
 * runs, n60hud): gdx3ds_nrr_draw's checks (level, an open mono main-target frame, the backend
 * latch) + NrrBegin on `c` (`seam` may be NULL: the caller drained its batch). 0 = refused, nothing
 * done. gdx3ds_nrr_close = NrrEnd. Between the two the category owns citro3d exactly as a draw
 * function does; it may open and close several times per frame (once per run). */
int gdx3ds_nrr_open(NrrCtx* c, void* seam, int cat);
void gdx3ds_nrr_close(NrrCtx* c);
/* nicourse ([native] ni_course): the same hand-off for a caller that is not an NRR category (no
 * per-category lever, no snapshot frame): needs the NRR master on, an open main-target frame and the
 * backend latch. `st` = the caller's own counters; `crumbCat` = the category its crumbs are filed
 * under. 0 = refused, nothing done; close with gdx3ds_nrr_close. */
int gdx3ds_nrr_open_ext(NrrCtx* c, void* seam, NrrCatStats* st, int crumbCat);

/* Hand-off with the owned interpreter's backend. NrrBegin draws the interpreter's open packed
 * batch (seam Flush: painter's order). NRR then owns the citro3d state: before it first changes a
 * state group (attribute layout, vertex buffers, effect = depth/blend/alpha test/cull/depth map,
 * viewport + scissor, TexEnv stages, TexEnv buffer = the fog mode, texture units, the projection
 * / gpuxf / xfon uniforms) the group is saved from citro3d's context. NrrEnd restores every saved
 * group and marks it dirty, so citro3d ends the hand-off holding exactly what the backend left
 * there, and every backend-cached state (shader + TexEnv constant mirror, attribute/xf state,
 * projection dedupe, gpuxf uniform shadow, vertex buffer / texture bind / alpha-test elision, fog,
 * depth/decal/blend, scissor, viewport) and the interpreter's render-state cache stay true by
 * construction: the next interpreter draw re-emits the restored groups, nothing is re-derived. */
void NrrBegin(NrrCtx* c);
void NrrEnd(NrrCtx* c);

/* ------------------------------------------------------------------ state blocks ---- */

enum {
    NRR_PROG_2D = 0, /* packed layout (pos4 uv2 rgba4 uv2, GDX3DS_VBOPACK_STRIDE floats), the
                        fixup projection, xfon = false: DrawPackedLean's program */
    NRR_PROG_XF = 1, /* gpuxf: raw 16-byte Vtx (s16 ob + flag, s16 st, u8 cn), xfon = true, the
                        28-vec4 uniform block (Gdx3dsXfDraw order) */
    NRR_PROG_XF_F = 2 /* gpuxf with float positions (24-byte stride, track_retained near-plane) */
};

enum { NRR_BLEND_OFF = 0, NRR_BLEND_ALPHA = 1 };
enum { NRR_CULL_NONE = 0, NRR_CULL_BACK = 1, NRR_CULL_FRONT = 2 };
enum { NRR_AT_OFF = 0, NRR_AT_EDGE = 1, NRR_AT_THRESHOLD = 2 };

#define NRR_TEX_NONE 0xFFFFFFFFu
/* [n60hud3] a texture id naming an atlas PAGE (NRR_TEX_PAGE | page index): the C3D_Tex every view
 * on that page binds. A category whose blocks sample many views of one page (the HUD's font
 * glyphs) keys one state block per page instead of one per view; the UVs carry the view. */
#define NRR_TEX_PAGE 0x80000000u

/* Everything a draw needs, in N64 / backend terms. Keys are compared bytewise: zero-fill them
 * (memset) before setting fields. Combiner: the backend's shader-pool ids (the interpreter's
 * cc id + options, gdx_cc) -- translated once by the backend's own MapCombiner. */
typedef struct NrrStateKey {
    uint64_t shaderId0;
    uint64_t shaderId1;
    uint32_t tex[2];       /* backend texture ids (NRR_TEX_NONE = unit unused) */
    uint8_t texLinear[2];  /* sampler filter */
    uint8_t texCms[2];     /* N64 G_TX_* clamp/mirror bits per axis */
    uint8_t texCmt[2];
    uint8_t program;       /* NRR_PROG_* */
    uint8_t depthTest;     /* N64 Z_CMP */
    uint8_t depthWrite;    /* N64 Z_UPD */
    uint8_t decal;         /* ZMODE_DEC: the backend's decal depth bias */
    uint8_t blend;         /* NRR_BLEND_* */
    uint8_t cull;          /* NRR_CULL_* (PICA cull; 2D is always NONE) */
    uint8_t alphaTest;     /* NRR_AT_* */
    uint8_t alphaRef;      /* NRR_AT_THRESHOLD reference (0-255) */
    uint8_t pad[2];
    int16_t scissor[4];    /* N64 window x, y, w, h; all 0 = full screen */
    /* [n60mach] a category-owned C3D_Tex per unit (NULL = resolve tex[u] through the backend).
     * The unit binds a copy of it with the key's filter/wrap; the category keeps it alive for as
     * long as it applies blocks built from this key (the machines own copies of their textures, so
     * a block never depends on the texture cache keeping an id). */
    const void* texObj[2];
} NrrStateKey;

/* Cold on a miss: translates the key into a resolved block (citro3d enums, TexEnv stages,
 * texture params) in a fixed table (NRR_STATE_MAX blocks; a full table returns NULL and counts
 * `stfull=`). The pointer stays valid until gdx3ds_nrr_state_flush (a texture deletion or a
 * course change: categories key their caches on it via NrrStateGeneration). */
#define NRR_STATE_MAX 128
const NrrState* NrrStateGet(const NrrStateKey* k);
/* [n60mach] The same cold build into category-owned memory of NrrStateBytes() bytes (8-aligned),
 * outside the shared table (a category whose block count is its own, e.g. one per machine run).
 * NULL = the shader does not translate. The block lives as long as the memory. */
uint32_t NrrStateBytes(void);
const NrrState* NrrStateBuildInto(const NrrStateKey* k, void* mem);
/* n60intro ([native] nrr_hud_late): `base` with only its scissor changed -- the words the backend's
 * SetScissor(x, y, w, h) would leave (all 0 = off), the direct words rebuilt -- into `mem`
 * (NrrStateBytes). The window rect is the backend's own ints, not the key's int16: a degenerate N64
 * scissor (lry < uly wraps the height) is carried as the stock rectangle would set it. */
const NrrState* NrrStateRescissor(const NrrState* base, void* mem, int x, int y, int w, int h);
uint32_t NrrStateGeneration(void);
void gdx3ds_nrr_state_flush(void);
/* Hot: field-group diffs against the last applied block -> citro3d calls. */
void NrrStateApply(NrrCtx* c, const NrrState* s);
/* Per-draw combiner inputs (1-based shader inputs, the interpreter's aux.inputRgba semantics:
 * input i = in[i-1], rgba 0..1): recomposes the stages whose constant rides an input. */
void NrrSetInputs(NrrCtx* c, const NrrState* s, const float (*in)[4], int n);
/* The fog-blend stage's constant (fog rgb 0..1), when the state has one (opt_fog shaders whose
 * alpha is opaque: the backend lerps fog by vertex alpha in a final TexEnv stage). */
void NrrSetFog(NrrCtx* c, const NrrState* s, const float rgb[3]);
/* The fog-blend TexEnv stage NrrSetFog writes, -1 when the shader has none (a fog shader without
 * one fogs through the PICA fog unit, which NRR does not drive: such a category keeps its fog). */
int NrrStateFogStage(const NrrState* s);
/* Which shader input rides the vertex colour / alpha (0 = none), for the vertex writers. */
int NrrStateVtxColorInput(const NrrState* s);
int NrrStateVtxAlphaInput(const NrrState* s);
/* Texture coordinate transform of unit `unit` (pow2 padding, atlas view offset):
 * u' = u * scale[0] + off[0], v' = v * scale[1] + off[1]. 0 = unit unused / not uploaded. */
int NrrStateTexXform(const NrrState* s, int unit, float scale[2], float off[2]);

/* ---------------------------------------------------------- streaming + drawing ---- */

/* Linear memory for this frame's streamed vertices / indices: carved from the backend's
 * per-frame packed pool (16-byte aligned), so the ONE data-cache flush per frame the backend
 * already does (FlushPendingVbo: EndFrame, and the [overlap] mid-frame kick's prepare) covers it.
 * NULL when the pool is out (counted; the category falls back). */
void* NrrRingAlloc(NrrCtx* c, uint32_t bytes);

/* Packed 2D triangles (NRR_PROG_2D): nVerts = 3 * tris, GDX3DS_VBOPACK_STRIDE floats each. */
void NrrDraw2D(NrrCtx* c, const float* verts, uint32_t nVerts);
/* The same packed layout for PERSPECTIVE content (clip w != 1: the sky, any CPU-transformed 3D):
 * the projection is the 3D scene map (gdx3ds_nrr_bk_proj3d: the fixup, the [display] border
 * mode's SCENE matrix, a [vpfix] fold) -- what DrawTriangles uploads for a non-UI draw. NrrDraw2D
 * takes the border mode's UI map (gdx3ds_nrr_bk_proj2d: the fixup in modes 0/1, zoomed in 2/3). [n60sky] */
void NrrDraw2DScene(NrrCtx* c, const float* verts, uint32_t nVerts);
/* n60stereo: the NEXT 2D draw is a prim-depth rectangle at NDC depth `primDepth` (0..1; the N64 prim
 * depth / 32767): in a stereo frame-replay frame, if its first vertex is screen-space (w == 1), it
 * takes the [anchor] class (the depth of the machine it labels) instead of screen depth -- the
 * per-draw loop's rule for the same rectangle. A no-op in mono. */
void NrrStereoAnchor(NrrCtx* c, float primDepth);
/* mstereo [stereo] menu_depth: the next 2D draw (`*verts`, `*nVerts` packed vertices) of a menu frame
 * takes its coverage layer's row (gdx3ds_stereo_menu_ui_row). A batch whose rectangles change layer
 * (gdx3ds_stereo_menu_run_end) is drawn here run by run, each at its own row, up to
 * its last run, and `*verts` / `*nVerts` are advanced to that last run, which the caller draws.
 * Cold; a no-op outside a stereo replay frame. */
void NrrStereoMenuRow(NrrCtx* c, float** verts, uint32_t* nVerts);
uint32_t gdx3ds_stereo_menu_run_end(const float* verts, uint32_t nVerts, uint32_t stride, uint32_t first);
/* 1 while the open frame takes the menu depth layout (gdx3ds_stereo.h). */
int gdx3ds_stereo_menu_frame(void);
/* gpuxf draw: upload the 28-vec4 uniform block (shadowed), bind verts, indexed draw. idx may
 * live in the ring or in resident linear memory. The projection uniform holds the backend's 3D
 * scene map for a mono draw (the fixup, or the border mode's scene matrix) -- [n60mach]: it used
 * to be left at whatever the last interpreter draw uploaded. */
void NrrDrawXf(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
               const float* unif28x4);
/* n60draw: NrrDrawXf with the gpuxf rows (bit i = vec4 i of the 28) that may differ from the block of
 * this caller's previous NrrDrawXf / NrrDrawXfRows in the same hand-off; every other row must be
 * bit-identical to it. With [native] nrr_draw bit 2 the direct core compares only those rows (the
 * hand-off's first block is always compared whole); otherwise the same as NrrDrawXf. */
void NrrDrawXfRows(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
                   const float* unif28x4, uint32_t rows);
/* nimach: the backend's viewport changed inside the hand-off (a menu machine's [vpfix] fold): the next
 * NrrDrawXf re-uploads gdx3ds_nrr_bk_proj3d() and, under keepVp, cuts the scissor to its clip square
 * again. Both emitters. */
void NrrProjInvalidate(NrrCtx* c);

/* --------------------------------------------------------------------- receipts ---- */

/* Render side, once per walked task (= rendered frame on renderthread=0; n64/gdx_rt_glue.cpp after
 * RunFrame): every 64 tasks `[nrr] frame=N tasks=64 | hud mk= dr= st= vx= us= fb= lag= | trk ...`,
 * per-frame averages, only the categories that are on. One int test with the master off. */
void gdx3ds_nrr_task_end(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NRR_H */
