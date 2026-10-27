/* gdx3ds_nrr_trk_game.c — n60 NRR track (milestone 1): the lean walk (gdx3ds_nrr_trk.h).
 *
 * Compiled with the game's headers. Replaces Course_Draw's far-to-near group walk
 * (course.c: the `while (true)` loop over sSegmentChunkGroups, Course_Draw{Forward,Backward}
 * ChunkGroup and the ten shape emitters) by the same state machine minus its output:
 *
 *  - every walk global is written exactly as the stock walk writes it (D_800F89D4, D_800F892C,
 *    sLastTrackShapeType, D_800F8930, D_800F8958[].chunk, D_800F89B8/BC/C0/C8, the working
 *    chunks, sWorkingChunkJoinInfo, gCourseVtxPtr's advance, every group's averageDepth), so
 *    the next stock walk and everything after Course_Draw see the stock values;
 *  - the texture-state GBI (gSPTexture pairs, LoadSync/SetTextureImage/LoadBlock) is emitted
 *    verbatim through the same emitters the shim uses (gdx3ds_track_gbi.c), so the interpreter's
 *    RDP state after the track is the stock one;
 *  - instead of gSPVertex / the template gSPDisplayList / gSPModifyVertex, each template op is
 *    one 8-byte record: the template and the RESIDENT rings in the two slot halves, named by
 *    the bake's own key (chunk, ring layout, U table, V choice, ST-patched);
 *  - no vertex is written to courseVtxBuffer (the GfxPool scratch the list would have read).
 *
 * The code follows course.c 4281-4480 and 3646-4280 statement for statement where it decides
 * something; the comments name the stock function each block mirrors. A tunnel-mouth join is an
 * op whose `join` names the chunk's resident join vertices (`[native] nrr_trk_joins`, n60trk2). A
 * group the bake cannot serve (a join with the lever off; a ring the bake lacks) is rewound and
 * handed to the stock group walk with the legacy recorder armed (gdx3ds_nrr_trk.h, "Fallback"). */
#include "global.h"
#include "fzx_course.h"

#include <stdint.h>

#include "nrr/gdx3ds_nrr_trk.h"
#include "gdx3ds_track_templates.h"

/* course.c's TU-local types (same layout). */
typedef struct NrrMaterial {
    void* texture;
    f32 textureScale;
    s32 width;
    s32 tile;
    s32 unk_10;
    s16 textureCoordinateMask;
    s16 u[5];
} NrrMaterial;

typedef struct NrrSlot {
    SegmentChunk* chunk;
    Gfx* tmpl[10]; /* unk_04 .. unk_28 */
    s32 loadVtxIndex;
} NrrSlot;

typedef struct NrrGroup {
    SegmentChunk* startChunk;
    SegmentChunk* endChunk;
    f32 averageDepth;
    s32 drawState;
} NrrGroup;

extern SegmentChunk gSegmentChunks[];
extern SegmentChunk* sLastSegmentChunk;
extern Vtx* gCourseVtxPtr;
extern s32 sLastTrackShapeType;
extern s32 D_800F892C;
extern s16 D_800F8930[5];
extern f32 D_800F894C;
extern f32 D_800F8950;
extern NrrSlot D_800F8958[2];
extern NrrSlot* D_800F89B8;
extern NrrSlot* D_800F89BC;
extern s32 D_800F89C0;
extern Gfx* sCourseDisp;
extern SegmentChunk* D_800F89C8;
extern SegmentChunk* sWorkingSegmentChunk;
extern SegmentChunk* sWorkingNextSegmentChunk;
extern s32 D_800F89D4;
extern bool D_800F89D8;
extern s32 sWorkingChunkJoinInfo;
extern NrrMaterial* D_800CF8C8[];
#ifdef EXPANSION_KIT
extern GfxPool* gGfxPool;
#endif

void Course_DrawForwardChunkGroup(void* chunkGroup);
void Course_DrawBackwardChunkGroup(void* chunkGroup);

void gdxtrk_gbi_texture(void* g, int s, int t, int level, int tile, int on);
void gdxtrk_gbi_loadsync(void* g);
void gdxtrk_gbi_settimg(void* g, int fmt, int siz, int width, const void* img);
void gdxtrk_gbi_loadblock(void* g, int tile, int uls, int ult, int lrs, int dxt);

/* Ring layouts (gdx3ds_track_bake_game.c): which setup a walking shape runs on its rings. */
enum { LAYOUT_ROAD = 0, LAYOUT_PIPE = 1, LAYOUT_CYL = 2, LAYOUT_HALF = 3 };

/* D_800F8958[slot].unk_04 .. unk_28 as gGdxTrkTemplate indices (course.c Course_Init 3308-3329). */
enum { F04 = 0, F08, F0C, F10, F14, F18, F1C, F20, F24, F28 };
static const uint8_t kTmpl[2][10] = {
    { 0, 2, 0, 4, 6, 7, 10, 11, 14, 16 },
    { 1, 3, 1, 5, 8, 9, 12, 13, 15, 17 },
};

enum { BAIL_NONE = 0, BAIL_JOIN, BAIL_MISS, BAIL_FULL, BAIL_TABLE, BAIL_LAYOUT };

/* What a slot half holds: the bake key of the ring the stock walk would have loaded there. */
typedef struct NrrHalf {
    s16 chunk; /* -1 = unknown (nothing the lean walk loaded) */
    u8 layout;
    u8 table;
    u8 corr;
    u8 patched;
} NrrHalf;

typedef struct NrrWalk {
    GdxNrrTrkFrame* f;
    const GdxNrrTrkIndex* ix;
    GdxNrrTrkWalkStats* st;
    NrrHalf half[2];
    s32 table; /* the U table id D_800F8930 holds, -1 unknown */
    s32 bail;
    s32 dry;
    uint32_t passBails; /* groups handed back this pass (verify compares only clean passes) */
} NrrWalk;

static NrrWalk sW;

/* <string.h> collides with the decomp's PR/leo.h macros in this TU: five-short helpers instead. */
static void CopyU(s16* dst, const s16* src) {
    int k;
    for (k = 0; k < 5; k++) {
        dst[k] = src[k];
    }
}

static int SameU(const s16* a, const s16* b) {
    int k;
    for (k = 0; k < 5; k++) {
        if (a[k] != b[k]) {
            return 0;
        }
    }
    return 1;
}

static int SameOb(const s16* a, const s16* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

/* ---- saved walk state (a group's rewind, and the verify's before/after) ------------------------ */

typedef struct NrrSaved {
    Gfx* disp;
    Vtx* vtxPtr;
    s32 lastShapeType, texTile, d4, joinInfo, c0;
    s16 u[5];
    SegmentChunk* slotChunk[2];
    NrrSlot* b8;
    NrrSlot* bc;
    SegmentChunk* c8;
    SegmentChunk* working;
    SegmentChunk* next;
    NrrHalf half[2];
    s32 table;
    uint32_t nOps, nRuns, runStart;
} NrrSaved;

static void SaveWalk(NrrSaved* s) {
    s->disp = sCourseDisp;
    s->vtxPtr = gCourseVtxPtr;
    s->lastShapeType = sLastTrackShapeType;
    s->texTile = D_800F892C;
    s->d4 = D_800F89D4;
    s->joinInfo = sWorkingChunkJoinInfo;
    s->c0 = D_800F89C0;
    CopyU(s->u, D_800F8930);
    s->slotChunk[0] = D_800F8958[0].chunk;
    s->slotChunk[1] = D_800F8958[1].chunk;
    s->b8 = D_800F89B8;
    s->bc = D_800F89BC;
    s->c8 = D_800F89C8;
    s->working = sWorkingSegmentChunk;
    s->next = sWorkingNextSegmentChunk;
    s->half[0] = sW.half[0];
    s->half[1] = sW.half[1];
    s->table = sW.table;
    s->nOps = sW.f->nOps;
    s->nRuns = sW.f->nRuns;
    s->runStart = sW.f->runStart;
}

static void RestoreWalk(const NrrSaved* s) {
    sCourseDisp = s->disp;
    gCourseVtxPtr = s->vtxPtr;
    sLastTrackShapeType = s->lastShapeType;
    D_800F892C = s->texTile;
    D_800F89D4 = s->d4;
    sWorkingChunkJoinInfo = s->joinInfo;
    D_800F89C0 = s->c0;
    CopyU(D_800F8930, s->u);
    D_800F8958[0].chunk = s->slotChunk[0];
    D_800F8958[1].chunk = s->slotChunk[1];
    D_800F89B8 = s->b8;
    D_800F89BC = s->bc;
    D_800F89C8 = s->c8;
    sWorkingSegmentChunk = s->working;
    sWorkingNextSegmentChunk = s->next;
    sW.half[0] = s->half[0];
    sW.half[1] = s->half[1];
    sW.table = s->table;
    sW.f->nOps = s->nOps;
    sW.f->nRuns = s->nRuns;
    sW.f->runStart = s->runStart;
}

/* ---- the snapshot ----------------------------------------------------------------------------- */

/* A material boundary: the open run gets its marker (as the recorder's gdxtrk_state does). */
static void CloseRun(void) {
    GdxNrrTrkFrame* f = sW.f;
    if (f->nOps == f->runStart) {
        return;
    }
    if (f->nRuns >= GDX_NRR_TRK_MAX_RUNS) {
        sW.bail = BAIL_FULL;
        return;
    }
    GdxNrrTrkRun* r = &f->runs[f->nRuns++];
    r->opStart = (uint16_t) f->runStart;
    r->opEnd = (uint16_t) f->nOps;
    r->shapeType = (uint16_t) sLastTrackShapeType;
    r->tile = (uint8_t) D_800F892C;
    r->pad = 0;
    f->runStart = f->nOps;
    gdxtrk_nrr_marker(sCourseDisp++);
    sW.st->runs++;
}

static uint16_t RingOf(const NrrHalf* h) {
    const GdxNrrTrkIndex* ix = sW.ix;
    uint32_t k;
    uint16_t r;
    if (h->chunk < 0 || (uint32_t) h->chunk >= ix->nChunks || h->table >= ix->nTables) {
        return GDX_NRR_TRK_NO_RING;
    }
    k = (((uint32_t) h->chunk * 4u + h->layout) * ix->nTables + h->table) * GDX_NRR_TRK_VMODES +
        (h->patched == 2 ? 4u + h->corr : h->corr * 2u + h->patched);
    r = ix->ring[k];
    return r;
}

/* gSPDisplayList(template): one op over the two halves as they are now. */
static void Op(int tmpl) {
    GdxNrrTrkFrame* f = sW.f;
    GdxNrrTrkOp* op;
    const uint16_t r0 = RingOf(&sW.half[0]);
    const uint16_t r1 = RingOf(&sW.half[1]);
    if (r0 == GDX_NRR_TRK_NO_RING || r1 == GDX_NRR_TRK_NO_RING) {
        sW.bail = BAIL_MISS;
        return;
    }
    if (f->nOps >= GDX_NRR_TRK_MAX_OPS) {
        sW.bail = BAIL_FULL;
        return;
    }
    op = &f->ops[f->nOps++];
    op->ring[0] = r0;
    op->ring[1] = r1;
    op->tmpl = (uint8_t) tmpl;
    op->pad = 0;
    op->join = GDX_NRR_TRK_NO_RING;
    sW.st->ops++;
}

/* Course_SetupChunkPipeTunnelJoinVtx(chunk) + gSPDisplayList(join template) + the 8-vertex reload
 * (n60trk2, `[native] nrr_trk_joins`). The join setup rewrites the slot's pending gSPVertex(8) into
 * gSPVertex(5, 16) and advances gCourseVtxPtr by 5 before the reload; the join template reads the
 * five join vertices and the matched slot's ring (`h`) only. The lean walk already named the reloaded
 * ring in the other half (Load), which the template does not read. Off: the group falls back. */
static void JoinOp(SegmentChunk* chunk, int tmpl) {
    const uint16_t* joinRing = sW.ix->joinRing;
    const uint32_t c = (uint32_t) (chunk - gSegmentChunks);
    GdxNrrTrkFrame* f = sW.f;
    if (joinRing == NULL) {
        sW.bail = BAIL_JOIN;
        return;
    }
    if (c >= sW.ix->nChunks || joinRing[c] == GDX_NRR_TRK_NO_RING) {
        sW.bail = BAIL_MISS;
        return;
    }
    gCourseVtxPtr += 5;
    Op(tmpl);
    if (sW.bail != BAIL_NONE) {
        return;
    }
    f->ops[f->nOps - 1].join = joinRing[c];
    sW.st->joins++;
}

/* The join op, then the template over the reloaded pair (both read after the 8-vertex reload). */
static void JoinThen(SegmentChunk* chunk, int joinTmpl, int pairTmpl) {
    JoinOp(chunk, joinTmpl);
    if (sW.bail == BAIL_NONE) {
        Op(pairTmpl);
    }
}

/* gSPVertex(gCourseVtxPtr, 8, half * 8) + Course_SetupChunk*Vtx(chunk, corr): the ring the
 * setup will write there (the list reads it after the setup ran). */
static void Load(int h, SegmentChunk* chunk, int layout, int corr) {
    NrrHalf* d = &sW.half[h];
    if (sW.table < 0) {
        sW.bail = BAIL_TABLE;
        return;
    }
    d->chunk = (s16) (chunk - gSegmentChunks);
    d->layout = (u8) layout;
    d->table = (u8) sW.table;
    d->corr = corr != 0 ? 1 : 0;
    d->patched = 0;
    gCourseVtxPtr += 8;
    sW.st->loads++;
}

/* The eight gSPModifyVertex(G_MWO_POINT_ST) of a backward (correction V) or forward (coordinate V)
 * patch: the bake's patched variant of this shape's layout under the table in force. */
/* Road, cylinder and half-pipe rings share Course_SetupChunkDefaultVtxPos's positions; the pipe
 * and tunnel ring has its own vertex order. */
static int PositionClass(int layout) {
    return layout == LAYOUT_PIPE ? 1 : 0;
}

static void Patch(int h, int layout, int corr) {
    NrrHalf* d = &sW.half[h];
    const int cross = PositionClass(d->layout) != PositionClass(layout);
    d->layout = (u8) layout; /* the patch writes this layout's U/V over the loaded positions */
    if (sW.table < 0) {
        sW.bail = BAIL_TABLE;
        return;
    }
    d->table = (u8) sW.table;
    d->corr = corr != 0 ? 1 : 0;
    d->patched = cross ? 2 : 1; /* 2: the other position class's vertex order (bake vmode 4 + corr) */
}

/* gSPTexture(off old) + gSPTexture(on new) when the tile changes (the shell's tile + 1 included). */
static void TexTile(s32 tile) {
    if (D_800F892C == tile) {
        return;
    }
    CloseRun();
    gdxtrk_gbi_texture(sCourseDisp++, 0, 0, 0, D_800F892C, G_OFF);
    gdxtrk_gbi_texture(sCourseDisp++, 0x8000, 0xFFFF, 0, tile, G_ON);
    D_800F892C = tile;
    sW.st->texSwitch++;
}

static int Flag80(const SegmentChunk* c) {
    return (c->trackSegmentInfo & TRACK_FLAG_80000000) != 0;
}

/* ---- the shape emitters (course.c 3646-4280) ---------------------------------------------------- */

/* Course_Setup*ChunkGroupPairVtx: gSPVertex(16, 0), then both rings. */
static void LoadPair(int layout, int workingCorr) {
    Load(0, sWorkingSegmentChunk, layout, workingCorr);
    Load(1, sWorkingNextSegmentChunk, layout, 0);
    D_800F8958[0].chunk = sWorkingSegmentChunk;
    D_800F8958[1].chunk = sWorkingNextSegmentChunk;
}

/* Common head of every reuse branch: B8 = the slot whose chunk matched, the parity flips. */
static int FlipSlot(void) {
    const int b8 = D_800F89C0;
    D_800F89B8 = &D_800F8958[b8];
    D_800F89C0 ^= 1;
    D_800F89BC = &D_800F8958[D_800F89C0];
    return b8;
}

static void RoadPair(void) {
    LoadPair(LAYOUT_ROAD, Flag80(sWorkingSegmentChunk));
    Op(0);
}

static void RoadBackward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingSegmentChunk == D_800F89B8->chunk) {
        const int b8 = D_800F89C0;
        if (Flag80(sWorkingSegmentChunk)) {
            Patch(b8, LAYOUT_ROAD, 1);
        }
        FlipSlot();
        Load(D_800F89C0, sWorkingNextSegmentChunk, LAYOUT_ROAD, 0);
        Op(kTmpl[b8][F04]);
        D_800F89BC->chunk = sWorkingNextSegmentChunk;
    } else {
        D_800F89C0 = 1;
        RoadPair();
    }
}

static void RoadForward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingNextSegmentChunk == D_800F89B8->chunk) {
        const int b8 = D_800F89C0;
        if (Flag80(sWorkingNextSegmentChunk)) {
            Patch(b8, LAYOUT_ROAD, 0);
        }
        FlipSlot();
        Load(D_800F89C0, sWorkingSegmentChunk, LAYOUT_ROAD, Flag80(sWorkingSegmentChunk));
        Op(kTmpl[D_800F89C0][F04]);
        D_800F89BC->chunk = sWorkingSegmentChunk;
    } else {
        D_800F89C0 = 0;
        RoadPair();
    }
}

/* The interior (tile) and, with the shell on, the exterior (tile + 1) of a pipe/tunnel op. */
static void PipeShell(int interior, int exterior) {
    TexTile(D_800F89D4);
    Op(interior);
    if (D_800F89D8) {
        TexTile(D_800F89D4 + 1);
        Op(exterior);
    }
}

static void PipePair(void) {
    LoadPair(LAYOUT_PIPE, Flag80(sWorkingSegmentChunk));
    if (sWorkingChunkJoinInfo != 0) {
        TexTile(D_800F89D4);
        switch (sWorkingSegmentChunk->trackSegmentInfo & TRACK_CHUNK_JOIN_TRANSITION_START_MASK) {
            case TRACK_CHUNK_JOIN_TRANSITION_START_NONE:
                Op(0);
                break;
            case TRACK_CHUNK_JOIN_PREVIOUS_END:
                Op(6);
                break;
            default:
                Op(7);
                break;
        }
    } else {
        PipeShell(2, 0);
    }
}

static void PipeBackward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingSegmentChunk == D_800F89B8->chunk) {
        const int b8 = D_800F89C0;
        if (Flag80(sWorkingSegmentChunk)) {
            Patch(b8, LAYOUT_PIPE, 1);
        }
        FlipSlot();
        Load(D_800F89C0, sWorkingNextSegmentChunk, LAYOUT_PIPE, 0);
        if (sWorkingChunkJoinInfo != 0) {
            TexTile(D_800F89D4);
            switch (sWorkingSegmentChunk->trackSegmentInfo & TRACK_CHUNK_JOIN_TRANSITION_START_MASK) {
                case TRACK_CHUNK_JOIN_TRANSITION_START_NONE:
                    Op(kTmpl[b8][F04]);
                    break;
                case TRACK_CHUNK_JOIN_PREVIOUS_END:
                    Op(kTmpl[b8][F14]);
                    break;
                default:
                    JoinThen(sWorkingSegmentChunk, kTmpl[b8][F20], kTmpl[b8][F18]);
                    break;
            }
        } else {
            PipeShell(kTmpl[b8][F08], kTmpl[b8][F04]);
        }
        D_800F89BC->chunk = sWorkingNextSegmentChunk;
    } else {
        D_800F89C0 = 1;
        PipePair();
    }
}

static void PipeForward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingNextSegmentChunk == D_800F89B8->chunk) {
        const int b8 = D_800F89C0;
        int bc;
        if (Flag80(sWorkingNextSegmentChunk)) {
            Patch(b8, LAYOUT_PIPE, 0);
        }
        FlipSlot();
        bc = D_800F89C0;
        Load(bc, sWorkingSegmentChunk, LAYOUT_PIPE, Flag80(sWorkingSegmentChunk));
        if (sWorkingChunkJoinInfo != 0) {
            TexTile(D_800F89D4);
            switch (sWorkingSegmentChunk->trackSegmentInfo & TRACK_CHUNK_JOIN_TRANSITION_START_MASK) {
                case TRACK_CHUNK_JOIN_TRANSITION_START_NONE:
                    Op(kTmpl[bc][F04]);
                    break;
                case TRACK_CHUNK_JOIN_PREVIOUS_END:
                    JoinThen(sWorkingNextSegmentChunk, kTmpl[b8][F1C], kTmpl[bc][F14]);
                    break;
                default:
                    Op(kTmpl[bc][F18]);
                    break;
            }
        } else {
            PipeShell(kTmpl[bc][F08], kTmpl[bc][F04]);
        }
        D_800F89BC->chunk = sWorkingSegmentChunk;
    } else {
        D_800F89C0 = 0;
        PipePair();
    }
}

/* The cylinder's template by TRACK_CHUNK_JOIN_TRANSITION_END of the working chunk. */
static int CylField(void) {
    switch (sWorkingSegmentChunk->trackSegmentInfo & TRACK_CHUNK_JOIN_TRANSITION_END_MASK) {
        case TRACK_CHUNK_JOIN_TRANSITION_END_NONE:
            return F04;
        case TRACK_CHUNK_JOIN_PREVIOUS_START:
            return F24;
        default:
            return F28;
    }
}

static void CylPair(void) {
    static const uint8_t kPair[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 14, 16 };
    LoadPair(LAYOUT_CYL, Flag80(sWorkingSegmentChunk));
    Op(kPair[CylField()]);
}

static void CylBackward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingSegmentChunk == D_800F89B8->chunk) {
        const int b8 = D_800F89C0;
        if (Flag80(sWorkingSegmentChunk)) {
            Patch(b8, LAYOUT_CYL, 1);
        }
        FlipSlot();
        Load(D_800F89C0, sWorkingNextSegmentChunk, LAYOUT_CYL, 0);
        Op(kTmpl[b8][CylField()]);
        D_800F89BC->chunk = sWorkingNextSegmentChunk;
    } else {
        D_800F89C0 = 1;
        CylPair();
    }
}

static void CylForward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingNextSegmentChunk == D_800F89B8->chunk) {
        const int b8 = D_800F89C0;
        if (Flag80(sWorkingNextSegmentChunk)) {
            Patch(b8, LAYOUT_CYL, 0);
        }
        FlipSlot();
        Load(D_800F89C0, sWorkingSegmentChunk, LAYOUT_CYL, Flag80(sWorkingSegmentChunk));
        Op(kTmpl[D_800F89C0][CylField()]);
        D_800F89BC->chunk = sWorkingSegmentChunk;
    } else {
        D_800F89C0 = 0;
        CylPair();
    }
}

static void HalfPair(void) {
    LoadPair(LAYOUT_HALF, Flag80(sWorkingSegmentChunk));
    Op(0);
}

static void HalfBackward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingSegmentChunk == D_800F89B8->chunk) {
        const int b8 = D_800F89C0;
        if (Flag80(sWorkingSegmentChunk)) {
            Patch(b8, LAYOUT_HALF, 1);
        }
        FlipSlot();
        Load(D_800F89C0, sWorkingNextSegmentChunk, LAYOUT_HALF, 0);
        Op(kTmpl[b8][F0C]);
        D_800F89BC->chunk = sWorkingNextSegmentChunk;
    } else {
        D_800F89C0 = 1;
        HalfPair();
    }
}

static void HalfForward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingNextSegmentChunk == D_800F89B8->chunk) {
        const int b8 = D_800F89C0;
        if (Flag80(sWorkingNextSegmentChunk)) {
            Patch(b8, LAYOUT_HALF, 0);
        }
        FlipSlot();
        Load(D_800F89C0, sWorkingSegmentChunk, LAYOUT_HALF, Flag80(sWorkingSegmentChunk));
        Op(kTmpl[D_800F89C0][F0C]);
        D_800F89BC->chunk = sWorkingSegmentChunk;
    } else {
        D_800F89C0 = 0;
        HalfPair();
    }
}

/* Course_SetupAirChunkGroupPairVtx: both rings with coordinate V. */
static void AirPair(void) {
    LoadPair(LAYOUT_ROAD, 0);
    Op(4);
}

static void AirBackward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingSegmentChunk == D_800F89B8->chunk) {
        const int b8 = FlipSlot();
        Load(D_800F89C0, sWorkingNextSegmentChunk, LAYOUT_ROAD, 0);
        Op(kTmpl[b8][F10]);
        D_800F89BC->chunk = sWorkingNextSegmentChunk;
    } else {
        D_800F89C0 = 1;
        AirPair();
    }
}

static void AirForward(void) {
    D_800F89B8 = &D_800F8958[D_800F89C0];
    if (sWorkingNextSegmentChunk == D_800F89B8->chunk) {
        FlipSlot();
        Load(D_800F89C0, sWorkingSegmentChunk, LAYOUT_ROAD, Flag80(sWorkingSegmentChunk));
        Op(kTmpl[D_800F89C0][F10]);
        D_800F89BC->chunk = sWorkingSegmentChunk;
    } else {
        D_800F89C0 = 0;
        AirPair();
    }
}

typedef void (*NrrEmitFn)(void);
static const NrrEmitFn kBackward[8] = { RoadBackward, RoadBackward, PipeBackward, CylBackward,
                                        HalfBackward, PipeBackward, AirBackward,  RoadBackward };
static const NrrEmitFn kForward[8] = { RoadForward, RoadForward, PipeForward, CylForward,
                                       HalfForward, PipeForward, AirForward,  RoadForward };

/* ---- the group walks (course.c Course_Draw{Backward,Forward}ChunkGroup) ------------------------ */

/* The material switch both walks run before the emitter: tile, texture load, U table. */
static void ChunkMaterial(const SegmentChunk* chunk, u32 trackShape) {
    const s32 trackType = chunk->trackSegmentInfo & TRACK_TYPE_MASK;
    const NrrMaterial* m;
    s32 shapeType;
    if (trackType == TRACK_TYPE_NONE) {
        return;
    }
    m = &D_800CF8C8[trackShape][trackType];
    D_800F89D4 = m->unk_10;
    TexTile(D_800F89D4);
    shapeType = chunk->trackSegmentInfo & (TRACK_SHAPE_MASK | TRACK_TYPE_MASK);
    if (shapeType != sLastTrackShapeType) {
        CloseRun();
        gdxtrk_gbi_loadsync(sCourseDisp++);
        gdxtrk_gbi_settimg(sCourseDisp++, G_IM_FMT_RGBA, G_IM_SIZ_16b, m->width, m->texture);
        gdxtrk_gbi_loadblock(sCourseDisp++, m->tile, 0, 0, 2047, CALC_DXT(m->width, G_IM_SIZ_16b));
        sLastTrackShapeType = shapeType;
        CopyU(D_800F8930, m->u);
        sW.table = sW.ix->chunkTable[chunk - gSegmentChunks];
        if (sW.table < 0) {
            sW.bail = BAIL_TABLE;
        }
    }
}

static int VtxBufferFull(void) {
#ifdef EXPANSION_KIT
    return gCourseVtxPtr >= &gGfxPool->courseVtxBuffer[0xFF0];
#else
    return 0;
#endif
}

static void EmitChunk(int forward) {
    const u32 trackShape = TRACK_SHAPE_INDEX((u32) sWorkingSegmentChunk->trackSegmentInfo & TRACK_SHAPE_MASK);
    ChunkMaterial(sWorkingSegmentChunk, trackShape);
    sWorkingChunkJoinInfo =
        sWorkingSegmentChunk->trackSegmentInfo &
        (TRACK_JOIN_MASK | TRACK_CHUNK_JOIN_TRANSITION_END_MASK | TRACK_CHUNK_JOIN_TRANSITION_START_MASK);
    if (forward) {
        kForward[trackShape & 7]();
    } else {
        kBackward[trackShape & 7]();
    }
}

static void WalkBackward(NrrGroup* g) {
    f32 var_fv0;
    sWorkingSegmentChunk = g->startChunk;
    sWorkingNextSegmentChunk = g->startChunk + 1;
    if (sWorkingNextSegmentChunk == sLastSegmentChunk) {
        sWorkingNextSegmentChunk = gSegmentChunks;
    }
    do {
        if (sWorkingNextSegmentChunk->trackSegmentInfo & TRACK_FLAG_CONTINUOUS) {
            if (sWorkingNextSegmentChunk != g->endChunk) {
                D_800F89C8 = sWorkingNextSegmentChunk + 1;
                if (D_800F89C8 == sLastSegmentChunk) {
                    D_800F89C8 = gSegmentChunks;
                }
                var_fv0 = D_800F894C;
                do {
                    var_fv0 += D_800F8950;
                    if (D_800F89C8->depth < var_fv0) {
                        break;
                    }
                    if ((sWorkingNextSegmentChunk = D_800F89C8) == g->endChunk) {
                        break;
                    }
                    D_800F89C8++;
                    if (D_800F89C8 == sLastSegmentChunk) {
                        D_800F89C8 = gSegmentChunks;
                    }
                } while (sWorkingNextSegmentChunk->trackSegmentInfo & TRACK_FLAG_CONTINUOUS);
            }
        }
        if (VtxBufferFull()) {
            break;
        }
        EmitChunk(0);
        if (sW.bail != BAIL_NONE) {
            return;
        }
        sWorkingSegmentChunk = sWorkingNextSegmentChunk++;
        if (sWorkingNextSegmentChunk == sLastSegmentChunk) {
            sWorkingNextSegmentChunk = gSegmentChunks;
        }
    } while (sWorkingSegmentChunk != g->endChunk);
}

static void WalkForward(NrrGroup* g) {
    f32 var_fv0;
    SegmentChunk* firstChunk = gSegmentChunks;
    sWorkingSegmentChunk = g->endChunk - 1;
    if (sWorkingSegmentChunk < firstChunk) {
        sWorkingSegmentChunk = sLastSegmentChunk - 1;
    }
    sWorkingNextSegmentChunk = g->endChunk;
    do {
        if (sWorkingSegmentChunk->trackSegmentInfo & TRACK_FLAG_CONTINUOUS) {
            if (sWorkingSegmentChunk != g->startChunk) {
                D_800F89C8 = sWorkingSegmentChunk - 1;
                if (D_800F89C8 < firstChunk) {
                    D_800F89C8 = sLastSegmentChunk - 1;
                }
                var_fv0 = D_800F894C;
                do {
                    var_fv0 += D_800F8950;
                    if (D_800F89C8->depth < var_fv0) {
                        break;
                    }
                    if ((sWorkingSegmentChunk = D_800F89C8) == g->startChunk) {
                        break;
                    }
                    D_800F89C8--;
                    if (D_800F89C8 < firstChunk) {
                        D_800F89C8 = sLastSegmentChunk - 1;
                    }
                } while (sWorkingSegmentChunk->trackSegmentInfo & TRACK_FLAG_CONTINUOUS);
            }
        }
        if (VtxBufferFull()) {
            break;
        }
        EmitChunk(1);
        if (sW.bail != BAIL_NONE) {
            return;
        }
        sWorkingNextSegmentChunk = sWorkingSegmentChunk--;
        if (sWorkingSegmentChunk < firstChunk) {
            sWorkingSegmentChunk = sLastSegmentChunk - 1;
        }
    } while (sWorkingNextSegmentChunk != g->startChunk);
}

/* ---- fallback: one group through the stock walk and the legacy recorder ------------------------ */

static void CountBail(int why) {
    GdxNrrTrkWalkStats* st = sW.st;
    st->fbGroups++;
    if (why == BAIL_JOIN) {
        st->fbJoin++;
    } else if (why == BAIL_MISS) {
        st->fbMiss++;
    } else if (why == BAIL_FULL) {
        st->fbFull++;
    } else if (why == BAIL_TABLE) {
        st->fbTable++;
    } else if (why == BAIL_LAYOUT) {
        st->fbLayout++;
    }
}

/* The halves' contents after a stock group are rings the lean walk did not name: force the next
 * group to load a fresh pair (both slot chunks NULL), as at the top of every pass. */
static void ForgetSlots(void) {
    D_800F8958[0].chunk = NULL;
    D_800F8958[1].chunk = NULL;
    sW.half[0].chunk = -1;
    sW.half[1].chunk = -1;
}

/* The U table D_800F8930 holds now (after a stock group, or at the top of the pass). */
static s32 TableOfU(void) {
    const GdxNrrTrkIndex* ix = sW.ix;
    uint32_t t;
    for (t = 0; t < ix->nTables; t++) {
        if (SameU(ix->tables[t], D_800F8930)) {
            return (s32) t;
        }
    }
    return -1;
}

static void LegacyGroup(NrrGroup* g) {
    CloseRun();
    ForgetSlots();
    gdxtrk_nrr_arm_legacy();
    if (g->drawState == 1) {
        Course_DrawForwardChunkGroup(g);
    } else {
        Course_DrawBackwardChunkGroup(g);
    }
    if (gdxtrk_nrr_disarm_legacy(sCourseDisp)) {
        sCourseDisp++;
    }
    ForgetSlots();
    sW.table = TableOfU();
    sW.f->fbGroups++;
}

static void WalkGroup(NrrGroup* g) {
    NrrSaved saved;
    SaveWalk(&saved);
    sW.bail = BAIL_NONE;
    if (g->drawState == 1) {
        WalkForward(g);
    } else {
        WalkBackward(g);
    }
    if (sW.bail == BAIL_NONE) {
        return;
    }
    CountBail(sW.bail);
    sW.passBails++;
    RestoreWalk(&saved);
    sW.bail = BAIL_NONE;
    if (sW.dry) {
        return; /* verify: the stock walk draws everything after us anyway */
    }
    LegacyGroup(g);
}

/* ---- the pass (course.c Course_Draw's `while (true)` loop) ------------------------------------- */

static NrrSaved sVerifyBefore;
static NrrSaved sVerifyAfter;
static int sVerifyArmed = 0;
static Gfx sDryDisp[2048];

int gdxtrk_nrr_game_walk(void* groups, void* groupsEnd, GdxNrrTrkFrame* f, const GdxNrrTrkIndex* ix, int dry,
                         GdxNrrTrkWalkStats* st) {
    NrrGroup* const begin = (NrrGroup*) groups;
    NrrGroup* const end = (NrrGroup*) groupsEnd;
    NrrGroup* furthest = NULL;
    NrrGroup* g;
    f32 var_fv1;
    f32 depths[64];
    int i;
    if (f == NULL || ix == NULL || begin == NULL || end <= begin || end - begin > 64) {
        return 0;
    }
    sW.f = f;
    sW.ix = ix;
    sW.st = st;
    sW.dry = dry;
    sW.bail = BAIL_NONE;
    sW.passBails = 0;
    sW.half[0].chunk = -1;
    sW.half[1].chunk = -1;
    sW.table = TableOfU();
    st->passes++;
    if (dry) {
        /* verify: everything below writes into scratch and is put back; the stock walk follows */
        for (g = begin, i = 0; g != end; g++, i++) {
            depths[i] = g->averageDepth;
        }
        SaveWalk(&sVerifyBefore);
        sCourseDisp = sDryDisp;
    }
    while (true) {
        var_fv1 = -10000.0f;
        g = begin;
        do {
            if (var_fv1 < g->averageDepth) {
                var_fv1 = g->averageDepth;
                furthest = g;
            }
            g++;
        } while (g != end);
        if (var_fv1 == -10000.0f) {
            break;
        }
        f->groups++;
        st->groups++;
        WalkGroup(furthest);
        furthest->averageDepth = -20000.0f;
        if (dry && sCourseDisp >= &sDryDisp[2048 - 64]) {
            sCourseDisp = sDryDisp; /* scratch: only the walk globals matter */
        }
    }
    CloseRun();
    if (dry) {
        SaveWalk(&sVerifyAfter);
        RestoreWalk(&sVerifyBefore);
        f->nOps = sVerifyAfter.nOps; /* the globals go back; the dry ops stay for the compare */
        f->nRuns = sVerifyAfter.nRuns;
        f->runStart = sVerifyAfter.runStart;
        for (g = begin, i = 0; g != end; g++, i++) {
            g->averageDepth = depths[i];
        }
        sVerifyArmed = 1;
        return 0; /* the stock loop runs next, recorded */
    }
    return 1;
}

/* ---- verify: the stock walk vs the dry lean walk ------------------------------------------------ */

/* The stock record's ops (gdx3ds_track_record.h, mirrored to keep this TU on the game headers). */
typedef struct NrrRecOp {
    uint8_t kind;
    uint8_t n;
    uint8_t slot;
    uint8_t tmpl;
    uint32_t arg;
} NrrRecOp;
typedef struct NrrRecHead {
    uint32_t magic, mode, frame;
    int32_t course;
    uint32_t nOps, nRuns;
    NrrRecOp ops[1];
} NrrRecHead;

static uint32_t CompareGlobals(void) {
    const NrrSaved* a = &sVerifyAfter;
    uint32_t bad = 0;
    bad += a->vtxPtr != gCourseVtxPtr;
    bad += a->lastShapeType != sLastTrackShapeType;
    bad += a->texTile != D_800F892C;
    bad += a->d4 != D_800F89D4;
    bad += a->joinInfo != sWorkingChunkJoinInfo;
    bad += a->c0 != D_800F89C0;
    bad += !SameU(a->u, D_800F8930);
    bad += a->slotChunk[0] != D_800F8958[0].chunk;
    bad += a->slotChunk[1] != D_800F8958[1].chunk;
    bad += a->b8 != D_800F89B8;
    bad += a->bc != D_800F89BC;
    bad += a->c8 != D_800F89C8;
    bad += a->working != sWorkingSegmentChunk;
    bad += a->next != sWorkingNextSegmentChunk;
    return bad;
}

/* One slot as the interpreter would read it: the loaded Vtx, then any ST patch (verbatim shorts). */
typedef struct NrrVSlot {
    const Vtx* v;
    uint32_t mod;
    int modified;
} NrrVSlot;

static uint32_t CompareRing(const NrrVSlot* s, const Vtx* ring, uint32_t* checked) {
    uint32_t bad = 0;
    int k;
    for (k = 0; k < 8; k++) {
        int16_t u;
        int16_t v;
        const Vtx* src = s[k].v;
        (*checked)++;
        if (src == NULL) {
            bad++;
            continue;
        }
        if (s[k].modified) {
            u = (int16_t) (s[k].mod >> 16);
            v = (int16_t) s[k].mod;
        } else {
            u = (int16_t) (((int) src->v.tc[0] * 0x8000) >> 16);
            v = (int16_t) (((int) src->v.tc[1] * 0xFFFF) >> 16);
        }
        if (!SameOb(src->v.ob, ring[k].v.ob) || u != ring[k].v.tc[0] ||
            v != ring[k].v.tc[1]) {
            bad++;
        }
    }
    return bad;
}

/* Which slot groups a template reads: bit 0 = slots 0-7, bit 1 = 8-15, bit 2 = the join slots 16-20.
 * A join template reads one ring and the join vertices; the other half holds whatever the stock walk
 * last loaded there (the join setup replaced its reload), which no triangle reads. */
static uint32_t TemplateReads(int tmpl) {
    const GdxTrkTemplate* t = &gGdxTrkTemplate[tmpl];
    uint32_t m = 0;
    int i;
    for (i = 0; i < t->ntris * 3; i++) {
        m |= 1u << (t->idx[i] >> 3);
    }
    return m;
}

/* The five join vertices: positions (and the flag/cn the bake seeded) as the stock walk loaded them
 * into slots 16-20. U/V are not compared: the stock values are stale courseVtxBuffer bytes and the
 * join templates' combiner reads no texel; `joinUv` counts how many differed (a receipt). */
uint32_t gdxtrk_nrr_verify_join_uv = 0;
uint32_t gdxtrk_nrr_verify_join_ops = 0;
static uint32_t CompareJoin(const NrrVSlot* s, const Vtx* join, uint32_t* checked) {
    uint32_t bad = 0;
    int k;
    for (k = 0; k < 5; k++) {
        const Vtx* src = s[k].v;
        (*checked)++;
        if (src == NULL || s[k].modified) {
            bad++;
            continue;
        }
        if (!SameOb(src->v.ob, join[k].v.ob) || src->v.cn[0] != join[k].v.cn[0] || src->v.cn[1] != join[k].v.cn[1] ||
            src->v.cn[2] != join[k].v.cn[2] || src->v.cn[3] != join[k].v.cn[3]) {
            bad++;
        }
        if (src->v.tc[0] != join[k].v.tc[0] || src->v.tc[1] != join[k].v.tc[1]) {
            gdxtrk_nrr_verify_join_uv++;
        }
    }
    return bad;
}

static uint32_t CompareOp(const NrrVSlot* slots, const GdxNrrTrkOp* lo, const Vtx* verts, uint32_t* checked) {
    const uint32_t reads = TemplateReads(lo->tmpl);
    uint32_t bad = 0;
    if (reads & 1u) {
        bad += CompareRing(&slots[0], &verts[lo->ring[0] * 8u], checked);
    }
    if (reads & 2u) {
        bad += CompareRing(&slots[8], &verts[lo->ring[1] * 8u], checked);
    }
    if (reads & 4u) {
        if (lo->join == GDX_NRR_TRK_NO_RING) {
            return bad + 1u;
        }
        bad += CompareJoin(&slots[16], &verts[lo->join * 8u], checked);
        gdxtrk_nrr_verify_join_ops++;
    }
    return bad;
}

uint32_t gdxtrk_nrr_verify_why_v = 0;
#define gdxtrk_nrr_verify_why gdxtrk_nrr_verify_why_v

uint32_t gdxtrk_nrr_game_verify(const void* rec, uint32_t opFrom, uint32_t opTo, uint32_t* checked,
                                uint32_t* globalsBad, uint32_t* opsCompared) {
    const NrrRecHead* r = (const NrrRecHead*) rec;
    const GdxNrrTrkFrame* f = sW.f;
    const Vtx* verts;
    NrrVSlot slots[32];
    uint32_t bad = 0;
    uint32_t lean = 0;
    uint32_t k;
    *checked = 0;
    *globalsBad = 0;
    *opsCompared = 0;
    gdxtrk_nrr_verify_why = (!sVerifyArmed ? 1u : 0u) | (r == NULL ? 2u : 0u) | (f == NULL ? 4u : 0u) |
                            (sW.ix == NULL ? 8u : 0u) | ((sW.ix != NULL && sW.ix->verts == NULL) ? 16u : 0u) |
                            (sW.passBails != 0 ? 32u : 0u);
    if (!sVerifyArmed || r == NULL || f == NULL || sW.ix == NULL || sW.ix->verts == NULL) {
        return 0;
    }
    sVerifyArmed = 0;
    if (sW.passBails != 0) {
        return 0; /* a rewound group changes everything after it: nothing to compare */
    }
    *globalsBad = CompareGlobals();
    verts = (const Vtx*) sW.ix->verts;
    for (k = 0; k < 32; k++) {
        slots[k].v = NULL;
        slots[k].mod = 0;
        slots[k].modified = 0;
    }
    for (k = opFrom; k < opTo && k < r->nOps; k++) {
        const NrrRecOp* op = &r->ops[k];
        if (op->kind == 1) {
            int j;
            for (j = 0; j < op->n && op->slot + j < 32; j++) {
                slots[op->slot + j].v = (const Vtx*) (uintptr_t) op->arg + j;
                slots[op->slot + j].modified = 0;
            }
        } else if (op->kind == 2) {
            if (op->slot < 32) {
                slots[op->slot].mod = op->arg;
                slots[op->slot].modified = 1;
            }
        } else if (op->kind == 3) {
            const GdxNrrTrkOp* lo;
            if (lean >= f->nOps) {
                bad++;
                continue;
            }
            lo = &f->ops[lean++];
            (*opsCompared)++;
            if (lo->tmpl != op->tmpl) {
                bad++;
                continue;
            }
            bad += CompareOp(slots, lo, verts, checked);
        }
    }
    bad += lean != f->nOps;
    return bad + *globalsBad;
}
