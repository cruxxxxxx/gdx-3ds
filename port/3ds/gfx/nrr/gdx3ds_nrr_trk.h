/* gdx3ds_nrr_trk.h — n60 NRR, the track category (milestone 1; docs/research/native/n60-design.md
 * §4.2, n60trk-progress.md).
 *
 * The course walk without the walk. Course_Draw keeps its cull and grouping (the gadget pass reads
 * the cull) and its stock setup lists (D_8014008/D_8014078, fog position, prim strobe, fog colour);
 * under `[native] nrr_trk` the decomp's far-to-near chunk-group walk is replaced by a lean walk
 * (port/3ds/game/gdx3ds_nrr_trk_game.c) that runs the walk's state machine -- merge LOD, slot
 * parity, templates, ST patches, the shell, texture switches, every walk global -- but writes no
 * vertex and records nothing: it emits the walk's own texture-state GBI (so the interpreter's RDP
 * state after the track is the stock one) and, per material run, one G_NOOP 0x63 marker whose w1
 * points at a run of template ops whose two rings are RESIDENT ring indices (the c5s3c bake, indexed
 * at load by (chunk, layout, U table, V choice, patched)). The render side
 * (gdx3ds_nrr_trk_draw.cpp) draws a run as one resident draw per material: indices only, gpuxf
 * shader, exact integer eye offset `xoff`, PICA clipping (performance mode).
 *
 * Joins. A tunnel-mouth join (Course_SetupChunkPipeTunnelJoinVtx: five extra vertices in slots 16-20,
 * positions from the chunk, U/V whatever courseVtxBuffer held) is drawn natively under
 * `[native] nrr_trk_joins=1`: the join vertices are baked per chunk (their positions are a function
 * of the chunk; the join templates' combiner, (PRIM - SHADE) * PRIM_A + SHADE / SHADE alpha, reads no
 * texel, so the stale U/V are dead inputs) and the op names them in `join`.
 *
 * Fallback. A group the lean walk cannot draw from the bake (a tunnel-mouth join with
 * nrr_trk_joins=0; a ring variant the bake lacks) is rewound and walked by the
 * decomp's own Course_Draw*ChunkGroup with the c5s3 recorder armed: the legacy retained path draws
 * that group (`[nrr] fallback cat=trk` receipt). 4P stays legacy entirely (folded viewports).
 *
 * Levers ([native], default 0, not under `all=1`):
 *   nrr_trk          0 off | 1 on | 2 verify (the lean walk runs dry, then the stock walk draws and
 *                    is compared: walk globals at the end of the pass + every template op's rings,
 *                    vertex for vertex against what the stock walk loaded)
 *   nrr_trk_maxerr   per-ring float24 bound gate, 1/1000 px (default 125); a ring over it is
 *                    counted (`f24over`), the census measures the real error
 *   nrr_trk_merge    (n60trkv) 1 = a frame's material runs in ONE hand-off: every run's marker but
 *                    the last is held by the core's peek (no NrrBegin/NrrEnd) and drawn in marker
 *                    order by the last one (gdx3ds_nrr_trk_draw.cpp "merge"); receipt `| merge
 *                    held/f= joined/f= lost= stale= drains=` (lost/stale/drains 0 in a race).
 *                    Off under nrr_trk_state=0 and the float24 receipts (nrr_trk_f24, xf_census)
 *   nrr_trk_atlas    (n60atlas, needs nrr_trk_merge) 1 = the course's track textures in up to three
 *                    1024x32 atlas pages: held runs that differ only by their texture share one draw
 *                    (gdx3ds_nrr_trk_draw.cpp "atlas"). TOLERANCE CLASS, not exact: the slot's column
 *                    is an offset of the interpolated coordinate (n60atlas-progress.md §2). Receipts
 *                    `[nrrtrk] atlas slot= / refuse / frame= runs/f= joined/f= copies= fail= draws/f=`
 * [debug] census levers (n60atlas, default 0): atlas_census (material census receipts),
 * atlas_probe (bind every material through a 2W copy at x = W: the offset-exactness experiment),
 * course_force (gdx3ds_autodrive.c: race course N).
 * Needs [native] track=1 track_retained=1 track_resident=1 (the bake) and interp=1. */
#ifndef GDX3DS_NRR_TRK_H
#define GDX3DS_NRR_TRK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Markers and dispatch are the NRR core's (gdx3ds_nrr.h): one 0x63 marker (cat NRR_CAT_TRK, view 0,
 * w1 = the core's NrrFrame, whose TRK root block points at the GdxNrrTrkFrame below) per material
 * run; the render side consumes the frame's runs in order, one per marker. */

#define GDX_NRR_TRK_MAGIC 0x4E525254u /* "NRRT" */
#define GDX_NRR_TRK_MAX_OPS 1024
#define GDX_NRR_TRK_MAX_RUNS 192

/* One template op: the template (gGdxTrkTemplate index) over the two rings in slots 0-7 / 8-15, and
 * for a tunnel-mouth join template (D_80143E0..D_80144D0, `[native] nrr_trk_joins`) the chunk's
 * resident join vertices in slots 16-20 (`join`; GDX_NRR_TRK_NO_RING for every other template). */
typedef struct GdxNrrTrkOp {
    uint16_t ring[2];
    uint8_t tmpl;
    uint8_t pad;
    uint16_t join;
} GdxNrrTrkOp;

struct GdxNrrTrkFrame;

/* A material run: the ops between two texture-state changes (one marker each, in order). */
typedef struct GdxNrrTrkRun {
    uint16_t opStart;
    uint16_t opEnd;
    uint16_t shapeType; /* sLastTrackShapeType: the course texture the walk loaded (TMEM) */
    uint8_t tile;       /* D_800F892C: the tile gSPTexture turned on */
    uint8_t pad;
} GdxNrrTrkRun;

/* The per-GfxPool snapshot (double-buffered by pool parity, like the track record). */
typedef struct GdxNrrTrkFrame {
    uint32_t magic;
    uint32_t frame;   /* gGameFrameCount */
    uint32_t gen;     /* the bake generation the ring indices belong to */
    int32_t course;
    uint32_t nOps;
    uint32_t nRuns;
    uint32_t runStart; /* first op of the open run (game side) */
    uint32_t fbGroups; /* groups walked by the legacy path this pass */
    uint32_t groups;   /* groups this pass */
    uint32_t views;    /* niview: the race's view count (1 = 1P; > 1 = a view of a multi-view race) */
    GdxNrrTrkOp ops[GDX_NRR_TRK_MAX_OPS];
    GdxNrrTrkRun runs[GDX_NRR_TRK_MAX_RUNS];
} GdxNrrTrkFrame;

/* ---- the bake's NRR index (game thread; gdx3ds_track_resident.cpp builds it) ---- */
#define GDX_NRR_TRK_MAX_TABLES 64
#define GDX_NRR_TRK_NO_RING 0xFFFFu
/* V modes per (chunk, layout, table): corr * 2 + patched (0-3), and 4 + corr = ST-patched with this
 * layout's formula over the OTHER position class's vertex order (pipe vs road/cylinder/half-pipe). */
#define GDX_NRR_TRK_VMODES 6u
typedef struct GdxNrrTrkIndex {
    uint32_t gen;
    uint32_t nChunks;
    uint32_t nTables;
    int16_t tables[GDX_NRR_TRK_MAX_TABLES][5];
    int8_t* chunkTable; /* the chunk's own U table (-1: TRACK_TYPE_NONE or too many tables) */
    uint16_t* ring;     /* [((chunk * 4 + layout) * nTables + table) * GDX_NRR_TRK_VMODES + vmode] */
    const void* verts;  /* verify only: a game-side copy of the resident vertices (Gdx3dsVtxIn) */
    uint32_t nRings;
    uint16_t* joinRing; /* [chunk]: the chunk's join vertices (nrr_trk_joins), NO_RING = none / NULL */
    uint32_t nJoinRings;
} GdxNrrTrkIndex;

/* The current course's index, NULL = none (lever off, nothing baked, or the bake failed). */
const GdxNrrTrkIndex* gdxtrk_nrr_index(void);

/* ---- levers (gdx3ds_track_sink.c) ---- */
int gdxtrk_nrr_level(void);
int gdxtrk_nrr_maxerr(void);
/* [native] nrr_trk_joins (default 0): tunnel-mouth join groups drawn by the lean walk (resident join
 * vertices) instead of the legacy group fallback. */
int gdxtrk_nrr_joins(void);

/* ---- game side: the sink's hooks for the lean walk (gdx3ds_track_sink.c) ---- */
/* Course_Draw, at the walk: 1 = the lean walk drew this pass (skip the stock loop). */
int gdxtrk_nrr_walk(void* groups, void* groupsEnd);
/* The pass's snapshot (NULL when the pass is not the NRR's). */
GdxNrrTrkFrame* gdxtrk_nrr_frame(void);
/* The 0x63 marker of the run just closed, written at `slot` (the core's writer). */
void gdxtrk_nrr_marker(void* slot);
/* The legacy recorder, armed for one group the lean walk hands back (and disarmed after it: the
 * open legacy run gets its marker at `slot`). */
void gdxtrk_nrr_arm_legacy(void);
int gdxtrk_nrr_disarm_legacy(void* slot);

/* ---- game side: the lean walk itself (port/3ds/game/gdx3ds_nrr_trk_game.c, game headers) ---- */
typedef struct GdxNrrTrkWalkStats {
    uint32_t passes, groups, ops, runs, texSwitch, loads;
    uint32_t fbGroups, fbJoin, fbMiss, fbFull, fbTable, fbLayout;
    uint32_t joins; /* join template ops drawn natively (nrr_trk_joins) */
} GdxNrrTrkWalkStats;
/* 1 = the walk was done by the lean walk (with any legacy groups inside it). `dry` = verify mode:
 * nothing is emitted into the real list, every walk global is left as it was, and the result is
 * kept for gdxtrk_nrr_game_verify after the stock walk. */
int gdxtrk_nrr_game_walk(void* groups, void* groupsEnd, GdxNrrTrkFrame* f, const GdxNrrTrkIndex* ix, int dry,
                         GdxNrrTrkWalkStats* st);
/* Verify: the stock walk just ran (recorded); compare its globals and its template ops against the
 * dry lean walk. Returns mismatches; *checked = values compared. */
uint32_t gdxtrk_nrr_game_verify(const void* rec, uint32_t opFrom, uint32_t opTo, uint32_t* checked,
                                uint32_t* globalsBad, uint32_t* opsCompared);

/* ---- render side (gdx3ds_nrr_trk_draw.cpp): gdx3ds_nrr_cat_init_trk registers the draw ---- */

/* Render-side counters for the receipt (render thread writes, the game side's receipt reads). */
typedef struct GdxNrrTrkDrawStats {
    uint32_t frames, runs, ops, tris, draws, resolves, events, genMiss, badRuns;
    uint32_t f24over, nearRings, census;
    uint32_t maxMpx;
    uint64_t ticks;
    uint64_t resolveTicks; /* the material bind + uniforms (Resolve) */
    uint64_t drawTicks;    /* the draws (NrrDrawXf, or the backend path) */
    uint32_t noState;      /* materials the core cannot express (they take the backend path) */
    uint32_t missGen, missTex; /* cached materials refused: core state flush, texture re-upload */
    uint32_t missScissor;      /* n60retry: refused: the backend's scissor moved since the block was built */
    uint32_t ownStates;        /* n60retry: blocks built into a material's own memory (core table full) */
    uint32_t vpSyncs;          /* n60retry: runs that put the walk's pending viewport / scissor on the backend */
    uint32_t held;         /* n60trkv merge: runs whose marker was held (drawn in a later hand-off) */
    uint32_t heldJoined;   /* held runs that joined the open draw (same material + constants) */
    uint32_t heldLost;     /* held runs dropped (their frame's last marker never came): must be 0 */
    uint32_t heldStale;    /* held runs whose texture was re-uploaded before the draw: must be 0 */
    uint32_t heldDrains;   /* hand-offs another category's marker forced on held runs: 0 in a race */
    /* n60atlas [debug] atlas_census: run-to-run material boundaries (see gdx3ds_nrr_trk_draw.cpp) */
    uint32_t cenFrames, cenRuns, cenSame, cenTexOnly, cenTexOnlyUv, cenTexOnlyDims, cenState, cenDiffOr;
    /* n60atlas [native] nrr_trk_atlas: held runs drawn through the atlas, those that joined the open
     * atlas draw, ring copies made, rings refused (outside the slot / no room) */
    uint32_t atlasRuns, atlasJoined, atlasCopies, atlasFail;
} GdxNrrTrkDrawStats;
extern GdxNrrTrkDrawStats gdx3ds_nrr_trk_stats;

#ifdef __cplusplus
}
#endif

#endif /* GDX3DS_NRR_TRK_H */
