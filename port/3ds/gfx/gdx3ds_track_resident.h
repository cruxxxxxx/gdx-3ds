/* gdx3ds_track_resident.h — [native] track_resident (NEXT.md C5 Stage 3c, c5s3c-progress.md).
 *
 * The course's ring vertices, baked ONCE per course load into a GPU-resident buffer, so the
 * retained replay (gdx3ds_track_retained.cpp) draws a template op of the game's walk by writing
 * its indices only: no per-vertex rebase, no per-triangle float24 bound, no stream writes.
 *
 * What is baked. Every vertex the walk writes into courseVtxBuffer for a chunk ring is a
 * function of (the chunk, the walking emitter's ring layout, the D_800F8930 U table in force,
 * coordinate or correction V, and whether an ST patch rewrote it). The bake enumerates, per
 * chunk, every layout and every table of the chunks within `track_resident_window` of it, both
 * V choices where the chunk carries TRACK_FLAG_80000000, and the patched form (the final U/V an
 * ST patch leaves) -- by running the decomp's own Course_SetupChunk*Vtx
 * (port/3ds/game/gdx3ds_track_bake_game.c) -- and keeps each distinct ring once. Positions are
 * the game's s16 WORLD coordinates; the shader adds the per-pass integer eye offset `xoff`
 * exactly (shader.v.pica), so the PICA sees the same eye-rebased vertex as the per-frame path.
 *
 * What stays the game's. The walk still runs and is still recorded: visibility, merge LOD,
 * forward/backward templates, the shell and the draw order are the game's own ops. A ring is
 * drawn from the resident buffer only when the ring the walk actually loaded is found there
 * byte for byte (positions, colour, final U/V); anything else stays on the per-frame path.
 *
 * Levers ([native], default off, not covered by `all=1`; needs track_retained=1):
 *   track_resident         0 off | 1 on
 *   track_resident_verify  0 | 1: re-check every resident triangle's vertices, as the GPU reads
 *                          them from the linear buffer, against the walk's emission (count only)
 *   track_resident_maxerr  per-ring float24 gate, 1/1000 px (default 125 = track_retained's)
 *   track_resident_window  chunks either side whose layouts/tables a ring may carry (default 32)
 * Receipts: `[trkres] bake ...` per course load, `[trkres] adopt ...` on the render side, and the
 * `res` block of the `[trkrr] frame=` line.
 *
 * Memory (ship6): ONE linear arena, reserved once at boot and reused by every bake: two course
 * slots (chunks, rings, hash and the vertices the GPU reads -- no per-course malloc, realloc or
 * linearAlloc, which fragmented the Old 3DS heap until a bake failed after ~6 course loads) and
 * the game thread's scratch (the bake's chunk table, the NRR index). A course past a slot's
 * capacity, or both slots busy, skips the bake: the previous course's set is dropped too (the
 * per-frame path draws), and `[trkres] arena ...` says so (an MP-budget line). Receipts:
 * `[trkres] arena reserved ...` at boot, `[trkres] bake ... slot=` per course. */
#ifndef GDX3DS_TRACK_RESIDENT_H
#define GDX3DS_TRACK_RESIDENT_H

#include <stdint.h>

#include "gdx3ds_vtx_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Game thread, Course_Init (gdxtrk_course_loaded): bake the current course. */
void gdx3ds_track_resident_bake(void);

/* ship6: boot (main_3ds.cpp, after the launch variants): latch the levers and, with
 * track_resident on, reserve the fixed linear arena every bake reuses (two course slots + the
 * game thread's bake scratch). The first bake reserves it instead when this was not called. */
void gdx3ds_track_resident_reserve(void);

/* Game-data side (port/3ds/game/gdx3ds_track_bake_game.c, compiled with the game's headers). */
int gdxtrk_game_chunk_count(void);
uint32_t gdxtrk_game_chunk_info(int i);
int gdxtrk_game_layout_of(uint32_t info);
int gdxtrk_game_chunk_table(int i, int16_t d[5]);
void gdxtrk_game_ring(int i, int layout, const int16_t d[5], int corr, int joinBits, const void* tmpl, void* out);
/* n60trk2: chunk i's five tunnel-mouth join vertices (Course_SetupChunkPipeTunnelJoinVtx), padded to 8. */
void gdxtrk_game_join_ring(int i, const void* tmpl, void* out);

#ifdef __cplusplus
}

/* Ring variant kinds (bit set), for the census of what the walk actually uses. */
enum : uint8_t {
    GDXTRK_RES_CORR = 1,      /* correction V (TRACK_FLAG_80000000 setup, or the backward patch) */
    GDXTRK_RES_PATCHED = 2,   /* the final U/V an ST patch leaves (V unscaled) */
    GDXTRK_RES_FTABLE = 4,    /* a U table other than the chunk's own */
    GDXTRK_RES_FLAYOUT = 8,   /* a ring layout other than the chunk's own shape's */
    GDXTRK_RES_JOIN = 16,     /* n60trk2: the five tunnel-mouth join vertices (slots 16-20), not a ring */
};

struct GdxTrkResRing {
    uint16_t chunk;
    uint8_t kind;
    uint8_t pad;
};

struct GdxTrkResChunk {
    uint32_t firstRing;
    uint16_t nRings;
    uint16_t info9; /* n60atlas: trackSegmentInfo & (TRACK_SHAPE_MASK | TRACK_TYPE_MASK) */
    float c[3];    /* bounding sphere of the ring positions (world) */
    float r;
    int16_t lo[3]; /* bounding box (world) */
    int16_t hi[3];
};

/* One course's bake. Immutable once published; owned by the render side after adoption.
 * ship6: the arrays live in one of the arena's two slots (gdx3ds_track_resident.cpp), never in
 * the heap; `verts` IS the GPU's linear buffer (GdxTrkResView.linear). */
struct GdxTrkResSet {
    int32_t slot;       /* ship6: the arena slot holding the arrays; -1 = the "clear" set (no bake) */
    uint32_t generation;
    int32_t course;
    uint32_t nChunks;
    uint32_t nRings;
    GdxTrkResChunk* chunks;
    GdxTrkResRing* rings;
    Gdx3dsVtxIn* verts; /* nRings * 8, ring-major; ob = world, tc = FINAL short U/V */
    uint32_t hashMask;
    uint16_t* hash;     /* chunk + 1 by ring-position hash (linear probing), 0 = empty */
    uint32_t maxVariants;
    uint32_t kindCount[16];
    float bakeMs;
};

/* The render side's view of the adopted bake (render thread only). */
struct GdxTrkResView {
    const GdxTrkResSet* set;
    const Gdx3dsVtxIn* linear; /* GPU copy of set->verts (linear memory) */
    uint32_t nVerts;
    uint32_t ringCap; /* n60atlas: rings the linear buffer holds (>= set->nRings; the rest is the
                         nrr_trk atlas's slot copies, written by the render side) */
};

namespace GdxTrkRes {

/* Lever state, latched once the ini is loaded. */
int Level();
bool Verify();
int MaxErr();

/* Render thread, at a retained run: adopt a newly published bake (upload + retire the old one).
 * NULL = no bake (lever off, nothing baked yet, or the upload failed). */
const GdxTrkResView* Acquire(uint32_t frame);

/* The chunk whose ring positions are exactly these 8 vertices' ob; -1 = none. */
int32_t FindChunk(const GdxTrkResSet& s, const Gdx3dsVtxIn* const v[8]);

/* Release every resident buffer (backend teardown); safe to call at any time on the render thread. */
void Shutdown();

} // namespace GdxTrkRes
#endif

#endif /* GDX3DS_TRACK_RESIDENT_H */
