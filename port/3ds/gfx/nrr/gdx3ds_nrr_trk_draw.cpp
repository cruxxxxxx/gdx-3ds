/* gdx3ds_nrr_trk_draw.cpp — n60 NRR track, the render side (gdx3ds_nrr_trk.h).
 *
 * At a G_NOOP 0x63 marker (cat 1) the owned interpreter hands over one material run of the lean
 * walk's snapshot. The run is drawn straight from the c5s3c resident buffer: per op the template's
 * indices over the two resident rings (no vertex is touched on the CPU), one gpuxf draw per
 * material, the pass matrix rebased to the integer eye origin with the exact `xoff` add in the
 * shader, PICA clipping at every plane (performance mode: no CPU near clipper).
 *
 * State. The interpreter's registers already hold the run's material (the walk's texture-state
 * GBI ran through the interpreter before the marker): the material is bound through
 * BindMaterialRec (course-scoped records, the machines_mattab proof) and its uniforms come from
 * gdx3ds_mesh_nrr_material, once per run and again after a template's combiner event.
 *
 * float24. Per chunk and pass: the ring sphere's planes give the smallest w (floored at the
 * near-plane w: the PICA clips there), and gpuxf's bound over the chunk's box gives the worst
 * screen error; over `nrr_trk_maxerr` it is counted (`f24over`). `[debug] xf_census=1` simulates
 * the float24 transform of every ring the PICA is handed. */
#include "gdx3ds_nrr.h"
#include "gdx3ds_nrr_internal.h" /* the backend bridge: current key, texture serial */
#include "gdx3ds_nrr_trk.h"

#include <cstddef>

#include <3ds.h>
#include <citro3d.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "../gdx3ds_interp_seam.h"
#include "../gdx3ds_mesh.h"
#include "../gdx3ds_track_resident.h"
#include "../gdx3ds_track_retained_math.h"
#include "../gdx3ds_track_templates.h"
#include "../gdx3ds_vbopack.h"
#include "../gdx3ds_vtx_math.h"
#include "../gdx3ds_xf_census.h"

extern "C" void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));

bool gdx3ds_mesh_nrr_material(GdxInterpSeam& g, GdxSeamMatRec* recs, int ways, int mode, const Gdx3dsVtxMathState& vs,
                              const Gdx3dsLoadedVtx& first, Gdx3dsXfDraw& xf, Gdx3dsVboPackAux& aux,
                              GdxSeamMatOutcome& o, GdxNrrMatInfo* info);
extern "C" int gdx3ds_native_raw(const char* key, int fallback) __attribute__((weak));
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact) __attribute__((weak));

namespace {

constexpr uint32_t kMaxIdx = 3072;
constexpr uint32_t kMaxChunks = 1024;
constexpr int kRecBuckets = 16;
constexpr int kRecWays = 4;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    if (gdx_dbg_logf == nullptr) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

/* The pass: the frame's camera as the interpreter holds it at the first marker. */
struct Pass {
    const GdxNrrTrkFrame* f;
    uint32_t frame;
    uint32_t serial;
    bool ok;
    Gdx3dsVtxMathState vs;
    int32_t origin[3];
    float mpRel[4][4];
    float xs;
    float nearW;
    float mvpT[4][4]; /* mpRel transposed: the uniform block's first four vec4s */
    GdxTrkRr::ClipXf clip;
    GdxTrkRr::RingNorms norms;
};
Pass sPass = {};

struct ChunkMark {
    uint32_t serial;
    uint8_t censused;
    uint8_t near; /* the sphere reaches the near plane: the PICA clips some of its triangles */
};
ChunkMark sChunk[kMaxChunks];


GdxSeamMatRec sRecs[kRecBuckets * kRecWays];
uint32_t sRecGen = 0;
/* [native] nrr_trk_state (default 1): materials as core state blocks + NrrDrawXf; 0 = every run
 * through the interpreter's bind and the backend's xf draw (the A/B and bisect lever). */
bool sUseState = true;
/* [native] nrr_trk_f24 (default 0) or [debug] xf_census: the per-chunk float24 bound + census. */
bool sDiag = false;
/* [native] nrr_trk_merge (n60trkv, default 0): a frame's material runs drawn in ONE hand-off (see
 * "merge" below); 0 = one hand-off per run (the n60trk path). */
bool sMerge = false;
/* [native] nrr_draw (n60draw, bit mask, default 0): bit 2 the uniform block is updated in the
 * material's own template and drawn with the rows that may have changed (NrrDrawXfRows); bit 8 the
 * pair-packed index append. Bits 1 and 4 are the core's. */
bool sXfRows = false;
bool sPairAppend = false;
bool sLeversLatched = false;
void PairEnsure(); /* n60draw: the pair tables (below) */
/* n60atlas: [debug] atlas_census (material census receipts, cold + one test per run) and
 * [debug] atlas_probe (every material's unit-0 texture bound as a 2W-wide atlas view at offset W:
 * the exactness experiment for an offset texture coordinate). Both default 0. */
int sCensus = 0;
int sProbe = 0;
/* [native] nrr_trk_atlas (n60atlas, default 0): the course's track textures in one atlas texture, so
 * held runs that differ only by their texture share one draw (see "atlas" below). Needs merge. */
bool sAtlasOn = false;

void LatchLevers() {
    if (sLeversLatched) {
        return;
    }
    sLeversLatched = true;
    sUseState = gdx3ds_native_raw == nullptr || gdx3ds_native_raw("nrr_trk_state", 1) != 0;
    sDiag = gdx3ds_xf_census_on != 0 || (gdx3ds_native_raw != nullptr && gdx3ds_native_raw("nrr_trk_f24", 0) != 0);
    sMerge = gdx3ds_native_raw != nullptr && gdx3ds_native_raw("nrr_trk_merge", 0) != 0 && sUseState && !sDiag;
    const int draw = gdx3ds_native_raw != nullptr ? gdx3ds_native_raw("nrr_draw", 0) : 0;
    sXfRows = (draw & 2) != 0;
    sPairAppend = (draw & 8) != 0;
    sAtlasOn = sMerge && gdx3ds_native_raw("nrr_trk_atlas", 0) != 0;
    sCensus = gdx3ds_config_get_int != nullptr ? gdx3ds_config_get_int("debug", "atlas_census", 0) : 0;
    sProbe = gdx3ds_config_get_int != nullptr ? gdx3ds_config_get_int("debug", "atlas_probe", 0) : 0;
    if (sPairAppend) {
        PairEnsure();
    }
}

bool sInitLogged = false;

/* ---- the pass ------------------------------------------------------------------------------- */

float XScaleOf(const Gdx3dsVtxMathState& vs) {
    if (vs.xMode == GDX3DS_VTX_X_STRETCH) {
        return 160.0f / (160.0f - 12.0f);
    }
    if (vs.xMode == GDX3DS_VTX_X_ADJ) {
        return (4.0f / 3.0f) / vs.aspect;
    }
    return 1.0f;
}

/* The integer eye origin and the matrix rebased to it (double compose), as track_retained's pass. */
__attribute__((noinline)) void ComposePass(Pass& p) {
    double eye[3] = { 0.0, 0.0, 0.0 };
    const bool haveEye = GdxTrkRr::SolveEye(p.vs.mp, eye);
    for (int k = 0; k < 3; k++) {
        const double e = haveEye ? std::floor(eye[k] + 0.5) : 0.0;
        p.origin[k] = e > 32767.0 ? 32767 : (e < -32768.0 ? -32768 : (int32_t)e);
    }
    memcpy(p.mpRel, p.vs.mp, sizeof(p.mpRel));
    for (int i = 0; i < 4; i++) {
        double t = p.vs.mp[3][i];
        for (int k = 0; k < 3; k++) {
            t += (double)p.origin[k] * (double)p.vs.mp[k][i];
        }
        p.mpRel[3][i] = (float)t;
    }
    p.xs = XScaleOf(p.vs);
    if (sDiag) { /* the float24 receipts' inputs only (CheckRing / CensusNearOp) */
        p.nearW = haveEye ? GdxTrkRr::NearPlaneW(p.vs.mp, eye) : 1.0f;
        for (int k = 0; k < 4; k++) {
            for (int i = 0; i < 4; i++) {
                p.clip.m[k][i] = p.mpRel[k][i];
            }
        }
        p.clip.xs = p.xs;
        GdxTrkRr::RingNormsOf(p.clip, &p.norms);
    }
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            p.mvpT[i][k] = p.mpRel[k][i];
        }
    }
}

__attribute__((noinline)) bool BeginPass(GdxInterpSeam& I, const GdxNrrTrkFrame* f) {
    if (sPass.f == f && sPass.frame == f->frame && sPass.serial != 0) {
        return sPass.ok;
    }
    sPass.f = f;
    sPass.frame = f->frame;
    sPass.serial++;
    sPass.ok = I.LatchVtxState(sPass.vs) && !sPass.vs.lighting;
    if (sPass.ok) {
        ComposePass(sPass);
    }
    gdx3ds_nrr_trk_stats.frames++;
    return sPass.ok;
}

/* ---- per chunk: float24 bound and census ------------------------------------------------------ */

__attribute__((noinline)) void CensusRing(const GdxTrkResSet& set, uint32_t ring) {
    const float(*m)[4] = sPass.mpRel;
    for (int k = 0; k < 8; k++) {
        const Gdx3dsVtxIn& v = set.verts[ring * 8u + k];
        int32_t r32[3];
        bool fits = true;
        for (int c = 0; c < 3; c++) {
            r32[c] = (int32_t)v.ob[c] - sPass.origin[c];
            fits = fits && r32[c] >= -32768 && r32[c] <= 32767;
        }
        if (!fits) {
            continue;
        }
        const int16_t r[3] = { (int16_t)r32[0], (int16_t)r32[1], (int16_t)r32[2] };
        const double x = ((double)r[0] * m[0][0] + (double)r[1] * m[1][0] + (double)r[2] * m[2][0] + m[3][0]) * sPass.xs;
        const double y = (double)r[0] * m[0][1] + (double)r[1] * m[1][1] + (double)r[2] * m[2][1] + m[3][1];
        const double w = (double)r[0] * m[0][3] + (double)r[1] * m[1][3] + (double)r[2] * m[2][3] + m[3][3];
        gdx3ds_xf_census_path = GDX3DS_XFC_TRK;
        gdx3ds_xf_census_note(r, m, sPass.xs, (float)x, (float)y, (float)w);
        gdx3ds_xf_census_path = GDX3DS_XFC_DL;
        gdx3ds_nrr_trk_stats.census++;
    }
}

/* The ring's chunk, once per pass: smallest w over its sphere (floored at the near plane, where
 * the PICA clips) and gpuxf's float24 bound over its box. */
void CheckRing(const GdxTrkResSet& set, uint32_t ring) {
    const uint32_t chunk = set.rings[ring].chunk;
    if (chunk >= kMaxChunks || sChunk[chunk].serial == sPass.serial) {
        return;
    }
    ChunkMark& mk = sChunk[chunk];
    mk.serial = sPass.serial;
    mk.censused = 0;
    mk.near = 0;
    const GdxTrkResChunk& ch = set.chunks[chunk];
    const double rel[3] = { (double)ch.c[0] - sPass.origin[0], (double)ch.c[1] - sPass.origin[1],
                            (double)ch.c[2] - sPass.origin[2] };
    double wMin = 0.0;
    double wCentre = 0.0;
    const int planes = GdxTrkRr::RingPlanes(sPass.clip, sPass.norms, rel, (double)ch.r, false, false, &wMin, &wCentre);
    if (planes == GdxTrkRr::RING_NEAR) {
        gdx3ds_nrr_trk_stats.nearRings++;
        mk.near = 1;
    }
    const double wFloor = wMin > (double)sPass.nearW ? wMin : (double)sPass.nearW;
    float a[3];
    for (int c = 0; c < 3; c++) {
        const float lo = std::fabs((float)((int32_t)ch.lo[c] - sPass.origin[c]));
        const float hi = std::fabs((float)((int32_t)ch.hi[c] - sPass.origin[c]));
        a[c] = lo > hi ? lo : hi;
    }
    const uint32_t bound = GdxTrkRr::RingBoundMpx(sPass.mpRel, sPass.xs, a, wFloor);
    if (bound > gdx3ds_nrr_trk_stats.maxMpx) {
        gdx3ds_nrr_trk_stats.maxMpx = bound;
    }
    const int maxErr = gdxtrk_nrr_maxerr();
    if (maxErr >= 0 && bound > (uint32_t)maxErr) {
        gdx3ds_nrr_trk_stats.f24over++;
    }
    if (gdx3ds_xf_census_on) {
        CensusRing(set, ring);
    }
}

/* The float24 receipts of one op (`[native] nrr_trk_f24=1` or `[debug] xf_census=1`): the two
 * rings' chunk bounds and, near the eye, the census of the PICA's clip points. Off the steady loop:
 * the bound is a receipt, not a gate (n60trk-progress.md §4: no render-side fallback exists once the
 * walk is skipped, and the analytic bound over-states the simulated error by two orders). */
void CensusNearOp(const GdxTrkResSet& set, const GdxTrkTemplate& t, const uint32_t base[3]);
__attribute__((noinline)) void DiagOp(const GdxTrkResSet& set, const GdxNrrTrkOp& op, const GdxTrkTemplate& t,
                                      const uint32_t base[3]) {
    CheckRing(set, op.ring[0]);
    CheckRing(set, op.ring[1]);
    if (op.join != GDX_NRR_TRK_NO_RING && gdx3ds_xf_census_on) {
        CensusRing(set, op.join); /* the join vertices (slots 16-20; 5-7 repeat vertex 0) */
    }
    if (gdx3ds_xf_census_on && (sChunk[set.rings[op.ring[0]].chunk & (kMaxChunks - 1u)].near ||
                                sChunk[set.rings[op.ring[1]].chunk & (kMaxChunks - 1u)].near)) {
        CensusNearOp(set, t, base);
    }
}

/* [debug] xf_census: every template edge of a near op that crosses the near plane, as the PICA's
 * clipper interpolates it in float24 (the `near` census path). */
__attribute__((noinline)) void CensusNearOp(const GdxTrkResSet& set, const GdxTrkTemplate& t, const uint32_t base[3]) {
    for (int i = 0; i < t.ntris * 3; i += 3) {
        for (int e = 0; e < 3; e++) {
            const uint8_t s0 = t.idx[i + e];
            const uint8_t s1 = t.idx[i + (e + 1) % 3];
            const Gdx3dsVtxIn& va = set.verts[base[s0 >> 3] + (s0 & 7u)];
            const Gdx3dsVtxIn& vb = set.verts[base[s1 >> 3] + (s1 & 7u)];
            float a[3];
            float b[3];
            for (int c = 0; c < 3; c++) {
                a[c] = (float)((int32_t)va.ob[c] - sPass.origin[c]);
                b[c] = (float)((int32_t)vb.ob[c] - sPass.origin[c]);
            }
            gdx3ds_xf_census_note_clip(a, b, sPass.mpRel, sPass.xs);
        }
    }
}

/* ---- materials -------------------------------------------------------------------------------
 *
 * A run's material is what the interpreter's registers hold at its marker (the walk's texture-state
 * GBI ran through the interpreter). It is resolved ONCE, cold, by the interpreter's own bind
 * (BindMaterialRec through course-scoped records, + gdx3ds_mesh_nrr_material for the gpuxf
 * uniforms), and the draw state the backend then holds is read back as a core state block
 * (gdx3ds_nrr_bk_current_key -> NrrStateGet). Every later run with the same material applies the
 * block (NrrStateApply), re-evaluates the per-frame constants from the registers (prim/env inputs:
 * the rail strobe; fog colour: the tunnel fade) and draws with NrrDrawXf: no interpreter bind, no
 * texture import, no backend draw path in the steady loop.
 *
 * Identity: (bake generation, the course texture the walk loaded = shapeType, the tile turned on,
 * combine, other modes, geometry mode) + the bound textures' content serials (a texture the
 * interpreter's cache evicted or re-uploaded misses, cold) + the core's state generation. */
constexpr int kMats = 24;
constexpr int kXfFloats = 28 * 4;

/* What names a material: compared as words (zero padding). */
struct MatKey {
    uint64_t combine;
    uint32_t oml, omh, geo;
    uint32_t gen;
    uint16_t shapeType;
    uint8_t tile;
    uint8_t pad;
    uint32_t hash; /* n60trkv: of the fields above (MatFind compares it first) */
};
static_assert(sizeof(MatKey) == 32, "MatKey compares as eight words");

/* One constant combiner input as the hot loop reads it: 4 register bytes, or one byte broadcast. */
struct InSrc {
    const uint8_t* p; /* nullptr: the input is not a register constant (0) */
    uint8_t bcast;    /* 1: p[0] into r, g, b (an alpha / LOD-fraction input) */
    uint8_t alpha;    /* the alpha source byte offset from p is meaningful (use_alpha) */
    uint8_t pad[2];
    const uint8_t* pa; /* alpha byte (nullptr: 0; useAlpha off: 1.0) */
};

struct Mat {
    MatKey key;
    InSrc in[7];
    uint32_t stateGen;
    uint32_t texId[2];
    uint32_t texSerial[2];
    const NrrState* st;  /* nullptr: the backend path (a fog shader the core cannot express) */
    int16_t scissor[4];  /* the block's scissor (NrrStateKey.scissor): valid while the backend's is this */
    int cullMode;        /* GDX3DS_CULL_* of the bind (the backend path's draw takes it too) */
    GdxNrrMatInfo info;
    Gdx3dsVboPackAux aux; /* the backend path's aux (and the cold draw) */
    NrrStateKey skey;     /* n60atlas census: the key the block was built from */
    uint8_t atlasSlot1;   /* n60atlas: course-atlas slot + 1 (0 = not in the atlas) */
    float atlasU;         /* n60atlas: xoff.w of this material's draws (32 on the atlas material, else 0) */
    alignas(8) float unif[kXfFloats]; /* Gdx3dsXfDraw mvp..off: mvp, fog, col, off set per draw */
};
Mat sMats[kMats];
int sMatNext = 0;
uint32_t sCold = 0;
/* n60retry: a Mat's own block memory (NrrStateBytes each, allocated on first need) for when the
 * core's shared table is full -- the post-race HUD screens fill it, and a material without a
 * block took the backend path and re-resolved every run. */
void* sMatOwnState[kMats];
/* The backend's scissor as a key field, read once per run after the walk's viewport / scissor
 * reached the backend (a cached block carries the scissor it was built under). */
int16_t sRunScissor[4];
/* niview: the run belongs to a view of a multi-view race (GdxNrrTrkFrame.views > 1): the draws keep
 * the walk's viewport/scissor (keepVp, cut to the view's clip square by the core, as the machines),
 * so a material is one Mat for every view (scissor key kMultiScissor, block scissor normalised). */
bool sMulti = false;
const int16_t kMultiScissor[4] = { -1, -1, -1, -1 };

struct Cur {
    const Mat* m;
    float in[7][4];
    float fog[3]; /* the fog stage's colour (useFog) */
    float fogA;   /* the fog colour's alpha: the uniform block's col.a under fogFactorConst */
    bool inputsOk;
};
Cur sCur = {};
/* n60draw: the material and pass of the previous NrrDrawXfRows (a Mat slot re-resolved or a new
 * course forgets it; a new hand-off compares its first block whole anyway). */
const Mat* sXfMat = nullptr;
uint32_t sXfPass = 0;
float sXfAtlasU = 0.0f; /* n60atlas: the xoff.w of that block */

/* ---- merge (n60trkv, `[native] nrr_trk_merge`) -------------------------------------------------
 *
 * Each material run closes with its own 0x63 marker, and each marker used to be one hand-off:
 * NrrBegin re-emitted the interpreter state the previous NrrEnd had marked dirty (C3Dgdx_FlushState),
 * then the run re-sent everything (viewport, the whole state block, projection, attributes, buffer,
 * all 28 gpuxf vec4s) for ONE draw. Hardware (hw-n60card3, hw-n60-3d2): ~147 us per marker against
 * ~0.08 us per index -- the track's cost was the hand-offs, not its vertices.
 *
 * Merged: a run's marker is HELD (the core's peek, no hand-off, no GPU word) when everything the run
 * reads from the interpreter can be read now -- its material is a hot hit (resolved before, block
 * built), its constants (prim/env inputs, fog colour, fog alpha) are copied from the registers as
 * they stand at its own marker, no viewport is pending, no template has a combiner event -- and the
 * frame's LAST run (or the first run that cannot be held) opens one hand-off that draws the held
 * runs in marker order and then itself. Consecutive held runs with the same material and the same
 * constants share one draw (same state, triangles appended in the same order: the same pixels).
 * No grouping by material across runs: an experiment (`nrr_trk_merge=2`, removed) grouped the held
 * runs by material when every held material was order-free (opaque, depth-tested, depth-written,
 * no decal) and never found such a frame on gp2-sector or Mute City (draws/f unchanged); the walk
 * orders its groups far to near, so marker order is kept.
 *
 * Between two markers of the walk the interpreter only runs the walk's texture-state GBI (gSPTexture,
 * LoadSync, SetTImg, LoadBlock: register and TMEM bookkeeping, no draw, no upload), and a frame with
 * a legacy fallback group (which draws between markers) is never held. */
constexpr int kHeldMax = 48;

struct HeldRun {
    const Mat* m;
    const GdxNrrTrkRun* run;
    uint32_t nIdx;
    int atlas;           /* n60atlas: atlas page + 1 the run draws through (its ops' rings in sOpRing), 0 = own */
    float fog[3];
    float fogA;
    float in[7][4];
};

struct Held {
    const NrrFrame* nf;
    uint32_t tick;
    const GdxNrrTrkFrame* f;
    int n;
    uint32_t nIdx;
    bool closeNow; /* the frame's last run was held: the next hand-off draws them all */
    HeldRun r[kHeldMax];
};
Held sHeld = {};

GdxSeamMatRec* RecsFor(const GdxSeamRegs& R) {
    const uint32_t h = (uint32_t)*R.first_tile_index * 0x9E3779B1u ^ (uint32_t)(*R.combine_mode >> 32) ^
                       (uint32_t)*R.combine_mode * 0x85EBCA6Bu;
    return &sRecs[((h >> 16) % kRecBuckets) * kRecWays];
}

void KeyOf(const GdxSeamRegs& R, const GdxNrrTrkRun& run, uint32_t gen, MatKey* k) {
    memset(k, 0, sizeof(*k));
    k->combine = *R.combine_mode;
    k->oml = *R.other_mode_l;
    k->omh = *R.other_mode_h;
    k->geo = *R.geometry_mode;
    k->gen = gen;
    k->shapeType = run.shapeType;
    k->tile = run.tile;
    uint32_t h = (uint32_t)k->combine * 0x9E3779B1u ^ (uint32_t)(k->combine >> 32) * 0x85EBCA6Bu;
    h ^= (k->oml * 0xC2B2AE35u) ^ (k->omh * 0x27D4EB2Fu) ^ (k->geo * 0x165667B1u);
    h ^= (k->gen << 24) ^ ((uint32_t)k->shapeType << 8) ^ k->tile;
    k->hash = h;
}

/* memcmp, not a uint32_t* walk: the key is written through its own fields (strict aliasing). */
bool SameKey(const MatKey& a, const MatKey& b) {
    return memcmp(&a, &b, sizeof(MatKey)) == 0;
}

__attribute__((noinline)) const Mat* NrrTrkHotMatFind(const GdxSeamRegs& R, const GdxNrrTrkRun& run, uint32_t gen) {
    MatKey k;
    KeyOf(R, run, gen, &k);
    for (int i = 0; i < kMats; i++) {
        const Mat& m = sMats[i];
        if (m.key.hash != k.hash || m.st == nullptr || !SameKey(m.key, k)) {
            continue;
        }
        if (m.stateGen != NrrStateGeneration()) {
            gdx3ds_nrr_trk_stats.missGen++;
            continue;
        }
        if (memcmp(m.scissor, sRunScissor, sizeof(sRunScissor)) != 0) {
            gdx3ds_nrr_trk_stats.missScissor++;
            continue; /* the game moved its scissor (a camera viewport transition): cold */
        }
        if ((m.texId[0] != NRR_TEX_NONE && gdx3ds_nrr_bk_tex_serial(m.texId[0]) != m.texSerial[0]) ||
            (m.texId[1] != NRR_TEX_NONE && gdx3ds_nrr_bk_tex_serial(m.texId[1]) != m.texSerial[1])) {
            gdx3ds_nrr_trk_stats.missTex++;
            continue; /* the interpreter's cache evicted or re-uploaded the texture: cold */
        }
        return &m;
    }
    return nullptr;
}

void MatInputs(const GdxSeamRegs& R, Mat& m);

/* Cold: the core block of a material key -- the shared table, else (full) the Mat's own memory. */
__attribute__((noinline)) const NrrState* MatState(const Mat& m, const NrrStateKey& k) {
    const NrrState* st = NrrStateGet(&k);
    if (st != nullptr) {
        return st;
    }
    const int slot = (int)(&m - sMats);
    if (sMatOwnState[slot] == nullptr) {
        sMatOwnState[slot] = ::operator new(NrrStateBytes(), std::nothrow);
    }
    if (sMatOwnState[slot] == nullptr) {
        return nullptr;
    }
    gdx3ds_nrr_trk_stats.ownStates++;
    return NrrStateBuildInto(&k, sMatOwnState[slot]);
}

/* ---- n60atlas: material census ([debug] atlas_census) and the atlas-view probe ([debug] atlas_probe)
 *
 * Census: every resolved material is logged once (`[atlascen] mat`: texture, sampler, combiner, depth,
 * blend, the texture-coordinate rows), and every run's material is compared with the previous run's
 * of the same pass (`[atlascen] frame=` per receipt window): same material / differs ONLY by the
 * texture (state key, combiner info and every uniform row but the unit's uva/uvc equal) / a state
 * change. A texture-only boundary is what one atlas would remove.
 *
 * Probe: the material's unit-0 texture is bound as a 2W-wide copy holding the texture at x = W
 * ([T, T] for REPEAT, [mirror(T), T] for MIRRORED_REPEAT: the same texel at every wrapped position)
 * and unit 0's u transform becomes u * 1/2 + 1/2. Same texels, same filter, same V: what changes is
 * only the interpolated coordinate's arithmetic -- the question an atlas has to answer first. */
constexpr int kRowTg = 17;
constexpr int kRowTcs = 18;
constexpr int kRowUva0 = 19;
constexpr int kRowUvc0 = 21;

uint32_t BppOf(int fmt) {
    static const uint8_t kBits[16] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 0, 0 };
    return kBits[fmt & 15];
}

uint32_t MortonOf(uint32_t x, uint32_t y) {
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) | ((y & 4u) << 3);
}

/* Columns [x0, x0 + w) of a tiled texture `dst` (width dw) from the tiled texture `src` (width w, the
 * same height and format), mirrored in x when `mirror`. Whole-byte texels; rows map 1:1. */
__attribute__((noinline, cold)) void TexCopyCols(uint8_t* dst, uint32_t dw, const uint8_t* src, uint32_t w, uint32_t h,
                                                 uint32_t bytes, uint32_t x0, bool mirror) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t sx = mirror ? (w - 1u - x) : x;
            const uint32_t so = (((y >> 3) * (w >> 3) + (sx >> 3)) * 64u + MortonOf(sx & 7u, y & 7u)) * bytes;
            const uint32_t dx = x0 + x;
            const uint32_t dof = (((y >> 3) * (dw >> 3) + (dx >> 3)) * 64u + MortonOf(dx & 7u, y & 7u)) * bytes;
            memcpy(dst + dof, src + so, bytes);
        }
    }
}

struct ProbeTex {
    uint32_t id;
    uint32_t serial;
    uint8_t mirror;
    C3D_Tex tex;
};
constexpr int kProbeMax = 48;
ProbeTex sProbeTex[kProbeMax];
int sProbeN = 0;
uint32_t sProbeBuilt = 0, sProbeClamp = 0, sProbeMip = 0, sProbeFmt = 0, sProbeView = 0;

__attribute__((noinline, cold)) const C3D_Tex* ProbeTexFor(uint32_t id, const C3D_Tex* src, bool mirror) {
    const uint32_t serial = gdx3ds_nrr_bk_tex_serial(id);
    for (int i = 0; i < sProbeN; i++) {
        ProbeTex& p = sProbeTex[i];
        if (p.id == id && p.serial == serial && p.mirror == (mirror ? 1 : 0)) {
            return &p.tex;
        }
    }
    if (sProbeN >= kProbeMax) {
        return nullptr; /* a measurement lever: the copies live for the session */
    }
    ProbeTex& p = sProbeTex[sProbeN];
    memset(&p, 0, sizeof(p));
    if (!C3D_TexInit(&p.tex, (u16)(src->width * 2u), src->height, src->fmt)) {
        return nullptr;
    }
    const uint32_t bytes = BppOf(src->fmt) / 8u;
    TexCopyCols(static_cast<uint8_t*>(p.tex.data), src->width * 2u, static_cast<const uint8_t*>(src->data), src->width,
                src->height, bytes, 0, mirror);
    TexCopyCols(static_cast<uint8_t*>(p.tex.data), src->width * 2u, static_cast<const uint8_t*>(src->data), src->width,
                src->height, bytes, src->width, false);
    gdx3ds_mesh_backend_flush_resident(p.tex.data, p.tex.size);
    p.tex.param = src->param;
    p.tex.border = src->border;
    p.tex.lodParam = src->lodParam;
    p.id = id;
    p.serial = serial;
    p.mirror = mirror ? 1 : 0;
    sProbeN++;
    sProbeBuilt++;
    return &p.tex;
}

/* The probe on a material being resolved: unit 0 through a 2W copy at x = W, u * 1/2 + 1/2. */
__attribute__((noinline, cold)) void MatProbe(Mat& m, NrrStateKey& k) {
    if (k.tex[0] == NRR_TEX_NONE || k.texObj[0] != nullptr) {
        return;
    }
    NrrBkTex bt;
    NrrBkShader sh;
    if (!gdx3ds_nrr_bk_tex(k.tex[0], &bt) || bt.tex == nullptr || !gdx3ds_nrr_bk_translate(k.shaderId0, k.shaderId1, &sh) ||
        !sh.usedTex[0]) {
        return;
    }
    const C3D_Tex* src = static_cast<const C3D_Tex*>(bt.tex);
    if (bt.uOff != 0.0f || bt.vOff != 0.0f) {
        sProbeView++;
        return;
    }
    if (sh.clamp[0][0] || (k.texCms[0] & 2u) != 0) {
        sProbeClamp++;
        return;
    }
    if (src->maxLevel != 0) {
        sProbeMip++;
        return;
    }
    if (BppOf(src->fmt) < 8u || src->width > 512u) {
        sProbeFmt++;
        return;
    }
    const C3D_Tex* t = ProbeTexFor(k.tex[0], src, (k.texCms[0] & 1u) != 0);
    if (t == nullptr) {
        return;
    }
    k.texObj[0] = t;
    float* uva = m.unif + 4 * kRowUva0;
    float* uvc = m.unif + 4 * kRowUvc0;
    uva[0] *= 0.5f;
    uva[2] *= 0.5f;
    uvc[0] *= 0.5f;
    uvc[2] = uvc[2] * 0.5f + 0.5f;
}

void CensusTex(char* b, int n, int* len, int unit, const NrrStateKey& k) {
    NrrBkTex bt;
    if (k.tex[unit] == NRR_TEX_NONE || !gdx3ds_nrr_bk_tex(k.tex[unit], &bt) || bt.tex == nullptr) {
        *len += snprintf(b + *len, (size_t)(n - *len), " | t%d none(%u)", unit, (unsigned)k.tex[unit]);
        return;
    }
    const C3D_Tex* t = static_cast<const C3D_Tex*>(bt.tex);
    *len += snprintf(b + *len, (size_t)(n - *len), " | t%d id=%u %ux%u fmt=%d lvl=%u lin=%u cm=%u,%u xf=%g,%g,%g,%g ser=%u", unit,
                     (unsigned)k.tex[unit], (unsigned)t->width, (unsigned)t->height, (int)t->fmt, (unsigned)t->maxLevel,
                     (unsigned)k.texLinear[unit], (unsigned)k.texCms[unit], (unsigned)k.texCmt[unit], (double)bt.uScale,
                     (double)bt.vScale, (double)bt.uOff, (double)bt.vOff, (unsigned)gdx3ds_nrr_bk_tex_serial(k.tex[unit]));
}

void CensusRow(char* b, int n, int* len, const char* name, const float* r) {
    *len += snprintf(b + *len, (size_t)(n - *len), " %s=%g,%g,%g,%g", name, (double)r[0], (double)r[1], (double)r[2],
                     (double)r[3]);
}

__attribute__((noinline, cold)) void CensusMat(const Mat& m, const NrrStateKey& k, const GdxNrrTrkRun& run) {
    if (gdx_dbg_logf == nullptr) {
        return;
    }
    char b[900];
    int len = snprintf(b, sizeof(b),
                       "[atlascen] mat course=%d gen=%u slot=%d shape=%u type=%u tile=%u | sh=%08lx%08lx:%08lx%08lx prog=%u "
                       "z=%u%u dec=%u bl=%u cull=%u at=%u/%u sc=%d,%d,%d,%d",
                       sPass.f != nullptr ? (int)sPass.f->course : -1, (unsigned)m.key.gen, (int)(&m - sMats),
                       (unsigned)(run.shapeType >> 6), (unsigned)(run.shapeType & 0x3Fu), (unsigned)run.tile,
                       (unsigned long)(k.shaderId0 >> 32), (unsigned long)k.shaderId0, (unsigned long)(k.shaderId1 >> 32),
                       (unsigned long)k.shaderId1, (unsigned)k.program, (unsigned)k.depthTest, (unsigned)k.depthWrite,
                       (unsigned)k.decal, (unsigned)k.blend, (unsigned)k.cull, (unsigned)k.alphaTest, (unsigned)k.alphaRef,
                       k.scissor[0], k.scissor[1], k.scissor[2], k.scissor[3]);
    CensusTex(b, (int)sizeof(b), &len, 0, k);
    CensusTex(b, (int)sizeof(b), &len, 1, k);
    len += snprintf(b + len, sizeof(b) - (size_t)len, " | in=%u ci=%d ai=%d cc=%u ac=%u fog=%u bc=%u ff=%u ua=%u |",
                    (unsigned)m.info.numInputs, (int)m.info.ci, (int)m.info.ai, (unsigned)m.info.colConst,
                    (unsigned)m.info.alphaConst, (unsigned)m.info.useFog, (unsigned)m.info.useBlendColor,
                    (unsigned)m.info.fogFactorConst, (unsigned)m.info.useAlpha);
    CensusRow(b, (int)sizeof(b), &len, "tg", m.unif + 4 * kRowTg);
    CensusRow(b, (int)sizeof(b), &len, "tcs", m.unif + 4 * kRowTcs);
    CensusRow(b, (int)sizeof(b), &len, "uva0", m.unif + 4 * kRowUva0);
    CensusRow(b, (int)sizeof(b), &len, "uva1", m.unif + 4 * (kRowUva0 + 1));
    CensusRow(b, (int)sizeof(b), &len, "uvc0", m.unif + 4 * kRowUvc0);
    CensusRow(b, (int)sizeof(b), &len, "uvc1", m.unif + 4 * (kRowUvc0 + 1));
    gdx_dbg_logf("%s probe=%u/%u/%u/%u/%u\n", b, sProbeBuilt, sProbeClamp, sProbeMip, sProbeFmt, sProbeView);
}

/* What two materials differ in (bits): 1 shaders, 2 texture ids, 4 sampler, 8 depth, 16 blend/alpha
 * test, 32 cull, 64 scissor, 128 program, 256 combiner info, 512 uniform rows but uva/uvc, 1024 the
 * uva/uvc rows, 2048 texture size / format / levels, 4096 texObj. */
__attribute__((noinline, cold)) uint32_t CensusDiff(const Mat& a, const Mat& b) {
    const NrrStateKey& x = a.skey;
    const NrrStateKey& y = b.skey;
    uint32_t d = 0;
    d |= (x.shaderId0 != y.shaderId0 || x.shaderId1 != y.shaderId1) ? 1u : 0u;
    d |= (x.tex[0] != y.tex[0] || x.tex[1] != y.tex[1]) ? 2u : 0u;
    d |= memcmp(x.texLinear, y.texLinear, 2) != 0 || memcmp(x.texCms, y.texCms, 2) != 0 || memcmp(x.texCmt, y.texCmt, 2) != 0
             ? 4u
             : 0u;
    d |= (x.depthTest != y.depthTest || x.depthWrite != y.depthWrite || x.decal != y.decal) ? 8u : 0u;
    d |= (x.blend != y.blend || x.alphaTest != y.alphaTest || x.alphaRef != y.alphaRef) ? 16u : 0u;
    d |= (x.cull != y.cull || a.cullMode != b.cullMode) ? 32u : 0u;
    d |= memcmp(x.scissor, y.scissor, sizeof(x.scissor)) != 0 ? 64u : 0u;
    d |= x.program != y.program ? 128u : 0u;
    d |= memcmp(&a.info, &b.info, sizeof(a.info)) != 0 ? 256u : 0u;
    for (int r = 0; r < 28; r++) {
        const bool uv = r >= kRowUva0 && r <= kRowUvc0 + 1;
        const bool pass = r <= 3 || r == 6 || r == 27 || r == 23; /* rewritten per draw */
        if (!pass && memcmp(a.unif + 4 * r, b.unif + 4 * r, 16) != 0) {
            d |= uv ? 1024u : 512u;
        }
    }
    for (int u = 0; u < 2; u++) {
        NrrBkTex ta;
        NrrBkTex tb;
        const bool ha = x.tex[u] != NRR_TEX_NONE && gdx3ds_nrr_bk_tex(x.tex[u], &ta) && ta.tex != nullptr;
        const bool hb = y.tex[u] != NRR_TEX_NONE && gdx3ds_nrr_bk_tex(y.tex[u], &tb) && tb.tex != nullptr;
        if (ha != hb) {
            d |= 2048u;
        } else if (ha) {
            const C3D_Tex* p = static_cast<const C3D_Tex*>(ta.tex);
            const C3D_Tex* q = static_cast<const C3D_Tex*>(tb.tex);
            d |= (p->width != q->width || p->height != q->height || p->fmt != q->fmt || p->maxLevel != q->maxLevel) ? 2048u
                                                                                                                 : 0u;
        }
    }
    d |= (x.texObj[0] != y.texObj[0] || x.texObj[1] != y.texObj[1]) ? 4096u : 0u;
    return d;
}

uint32_t sCenPass = 0;
const Mat* sCenPrev = nullptr;
char sCenSeq[160];
int sCenSeqLen = 0;
uint32_t sCenSeqFrames = 0;

/* One run's material against the previous run's of the same pass. */
__attribute__((noinline, cold)) void CensusRun(const Mat* m) {
    GdxNrrTrkDrawStats& st = gdx3ds_nrr_trk_stats;
    if (sCenPass != sPass.serial) {
        if (sCenSeqLen > 0 && gdx_dbg_logf != nullptr && (sCenSeqFrames++ % 128u) == 0) {
            gdx_dbg_logf("[atlascen] seq course=%d frame=%u%s\n", sPass.f != nullptr ? (int)sPass.f->course : -1,
                         sPass.frame, sCenSeq);
        }
        sCenPass = sPass.serial;
        sCenPrev = nullptr;
        sCenSeqLen = 0;
        st.cenFrames++;
    }
    st.cenRuns++;
    const Mat* p = sCenPrev;
    sCenPrev = m;
    const int slot = (int)(m - sMats);
    char tag = 'S';
    if (p == nullptr) {
        tag = '^';
    } else if (p == m) {
        st.cenSame++;
        tag = '=';
    } else {
        const uint32_t d = CensusDiff(*p, *m);
        if ((d & ~(2u | 1024u | 2048u)) == 0) {
            st.cenTexOnly++;
            st.cenTexOnlyUv += (d & 1024u) == 0 ? 1u : 0u;
            st.cenTexOnlyDims += (d & 2048u) == 0 ? 1u : 0u;
            tag = 't';
        } else {
            st.cenState++;
            st.cenDiffOr |= d;
        }
    }
    if (sCenSeqLen < (int)sizeof(sCenSeq) - 8) {
        sCenSeqLen += snprintf(sCenSeq + sCenSeqLen, sizeof(sCenSeq) - (size_t)sCenSeqLen, " %c%d", tag, slot);
    }
}

/* ---- n60atlas: the course texture atlas ([native] nrr_trk_atlas) ------------------------------------
 *
 * Census (n60atlas-progress.md §1): on every course the walk's material runs differ, run to run, ONLY by
 * the texture -- one combiner, one state, the same uniform rows but unit 0's uva/uvc; RGBA8, linear,
 * no mip levels, 64x32 / 128x16 / 64x16 (16- and 32-high), REPEAT (the tunnel shell: MIRRORED in T).
 *
 * The atlas: one 1024 x 32 texture of the group's format, one SLOT per (texture, wrap) at column x0,
 * 2W wide. Texel space is what the walk's coordinates already are: x = U/32 + 1/2, y = V/32 + 1/2
 * (uva0 = 1/32W, 1/32H, 1/2W, 1/2H on every admitted material), so on the atlas
 *   - V repeats natively: a slot holds its texture replicated to 32 rows (a MIRRORED-T texture as
 *     [T; mirror(T)], whose period IS the mirrored period) -- the same texel at every V, and the
 *     coordinate is the original one scaled by a power of two (exact);
 *   - U is unrolled over the two periods the walk's U tables span (x in [1.5, 254.5] for W=128,
 *     [1.5, 126.5] for 64: [T, T], or [T, mirror(T)] for MIRRORED-S): no wrap is needed in the slot,
 *     every ring is range-checked against it (0 <= U <= 32 (2W - 1));
 *   - the slot's column rides the vertex: `flag` (the position's w short, unread by every other
 *     draw) = x0, and the shader adds flag * xoff.w to U, xoff.w = 32 on the atlas draw only.
 * A ring is drawn by different slots (a texture boundary, the tunnel's interior and shell over the
 * same rings): the first slot of the chunk's own (shape, type) owns the bake's ring (its flag
 * rewritten once, before any atlas draw reads it); any other (ring, slot) is a copy of the ring's 8
 * vertices behind the bake's rings (GdxTrkResView.ringCap), made once, cold.
 * NOT EXACT: the interpolated coordinate is x + x0 instead of x; the host/PICA rounding of the
 * interpolation differs in the last bits (the `atlas_probe` experiment: 1-7 pixels per frame by one
 * 5-bit quantum on the canonical replay). A tolerance-class lever, see the progress doc §3. */
constexpr uint32_t kAtlasW = 1024;
constexpr uint32_t kAtlasH = 32;
constexpr int kAtlasSlots = 24;
constexpr int kAtlasPages = 3; /* a course whose slots pass 1024 columns opens another page (another draw) */
constexpr uint32_t kAtlasHash = 2048; /* (ring, slot) -> copy; power of two */

struct AtlasSlot {
    uint32_t texId;
    uint32_t serial;
    uint16_t x0;
    uint16_t w;      /* the texture's width (the slot is 2w) */
    uint16_t shapeType;
    uint8_t mirS;
    uint8_t mirT;
    uint8_t owner;   /* 1: the first slot of its shapeType: it owns the bake's rings of those chunks */
    uint8_t page;
};

struct AtlasPage {
    bool texOk;
    C3D_Tex tex;
    uint32_t xNext;
    bool haveMat;
    Mat mat; /* the draw material: the group's state with this page bound, atlasU = 32 */
};

struct Atlas {
    uint32_t gen;        /* bake generation the slots and copies belong to (0 = none) */
    int nPages;          /* pages in use this course (their texture memory is kept across courses) */
    AtlasPage page[kAtlasPages];
    int nSlots;
    AtlasSlot slot[kAtlasSlots];
    bool haveGroup;
    Mat group;           /* the first admitted material: what every member must share */
    const GdxTrkResView* view;
    uint32_t nBase;      /* the bake's rings */
    uint32_t ringNext;   /* next free copy ring */
    uint8_t* home;       /* per bake ring: owning slot + 1, 0 unassigned, 0xFF refused */
    uint32_t homeCap;
    uint32_t hashKey[kAtlasHash]; /* (ring << 8 | slot) + 1, 0 = empty */
    uint16_t hashVal[kAtlasHash];
    uint32_t copies, rejects;
    uint32_t dirtyLo, dirtyHi; /* ring copies written since the last flush (one flush per hand-off) */
};
Atlas sAtlas = {};
/* The atlas rings of the held runs' ops, by op index (sOpRing[k] = ring0, ring1, join). */
uint16_t sOpRing[GDX_NRR_TRK_MAX_OPS][3];

void AtlasLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void AtlasLog(const char* fmt, ...) {
    if (gdx_dbg_logf == nullptr) {
        return;
    }
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

/* A new course (bake generation): forget the slots, the copies and the ownership. The texture memory
 * is kept (a course change is many frames of loading: nothing samples the old content by then). */
__attribute__((noinline, cold)) void AtlasReset(uint32_t gen) {
    sAtlas.gen = gen;
    sAtlas.nPages = 0;
    for (int p = 0; p < kAtlasPages; p++) {
        sAtlas.page[p].xNext = 0;
        sAtlas.page[p].haveMat = false;
    }
    sAtlas.nSlots = 0;
    sAtlas.haveGroup = false;
    sAtlas.view = nullptr;
    sAtlas.nBase = 0;
    sAtlas.ringNext = 0;
    if (sAtlas.home != nullptr) {
        memset(sAtlas.home, 0, sAtlas.homeCap);
    }
    memset(sAtlas.hashKey, 0, sizeof(sAtlas.hashKey));
    sAtlas.copies = 0;
    sAtlas.rejects = 0;
}

bool IsPow2(uint32_t v) {
    return v != 0 && (v & (v - 1u)) == 0;
}

/* The texture-coordinate rows the atlas reproduces: unit 0 in texel units of a W x H texture
 * (x = U/32 + 1/2), no shader clamp, no offset; texgen off, plain short coordinates. */
bool AtlasRowsOk(const Mat& m, uint32_t w, uint32_t h) {
    const float* tg = m.unif + 4 * kRowTg;
    const float* tcs = m.unif + 4 * kRowTcs;
    const float* a = m.unif + 4 * kRowUva0;
    const float* c = m.unif + 4 * kRowUvc0;
    return tg[0] == 0.0f && tg[3] == 1.0f && tcs[0] == 1.0f && tcs[1] == 1.0f && a[0] == 1.0f / (32.0f * (float)w) &&
           a[1] == 1.0f / (32.0f * (float)h) && a[2] == 0.5f / (float)w && a[3] == 0.5f / (float)h && c[0] >= 1e17f &&
           c[1] >= 1e17f && c[2] == 0.0f && c[3] == 0.0f;
}

/* Everything but unit 0's texture, its wrap and its coordinate rows is the group's. */
bool AtlasSameGroup(const Mat& a, const Mat& b) {
    const NrrStateKey& x = a.skey;
    const NrrStateKey& y = b.skey;
    if (x.shaderId0 != y.shaderId0 || x.shaderId1 != y.shaderId1 || x.tex[1] != y.tex[1] || x.texLinear[0] != y.texLinear[0] ||
        x.program != y.program || x.depthTest != y.depthTest || x.depthWrite != y.depthWrite || x.decal != y.decal ||
        x.blend != y.blend || x.cull != y.cull || x.alphaTest != y.alphaTest || x.alphaRef != y.alphaRef ||
        memcmp(x.scissor, y.scissor, sizeof(x.scissor)) != 0 || a.cullMode != b.cullMode ||
        memcmp(&a.info, &b.info, sizeof(a.info)) != 0) {
        return false;
    }
    for (int r = 0; r < 28; r++) {
        const bool uv0 = r == kRowUva0 || r == kRowUvc0;
        const bool pass = r <= 3 || r == 6 || r == 23 || r == 27; /* rewritten per draw */
        if (!uv0 && !pass && memcmp(a.unif + 4 * r, b.unif + 4 * r, 16) != 0) {
            return false;
        }
    }
    return true;
}

/* Slot columns [x0, x0 + 2w) x all 32 rows from the texture (w x h): U unrolled over two periods
 * (mirrored second period for MIRRORED-S), V replicated to 32 rows (period 2h, mirrored second half,
 * for MIRRORED-T). Logical rows run from t = 0; PICA stores the image bottom-up (logical row r at
 * storage row H-1-r: gfx_citro3d.cpp's upload). Whole 8x8 tiles are copied when the tile maps
 * whole (no mirrored half), texel by texel otherwise; only the slot's tile spans are flushed. */
uint32_t AtlasSrcRow(uint32_t sa, uint32_t h, bool mirT) {
    const uint32_t period = mirT ? 2u * h : h;
    const uint32_t rho = kAtlasH - 1u - sa;                              /* logical atlas row */
    const uint32_t ph = rho % period;
    const uint32_t r = (mirT && ph >= h) ? (2u * h - 1u - ph) : (ph % h); /* logical texture row */
    return h - 1u - r;                                                   /* its storage row */
}

uint32_t AtlasSrcCol(uint32_t x, uint32_t w, bool mirS) {
    return x < w ? x : (mirS ? (2u * w - 1u - x) : (x - w));
}

__attribute__((noinline, cold)) void AtlasFill(AtlasPage& pg, const C3D_Tex* src, uint32_t x0, bool mirS, bool mirT) {
    const uint32_t w = src->width;
    const uint32_t h = src->height;
    const uint32_t bytes = BppOf(src->fmt) / 8u;
    const uint32_t tileBytes = 64u * bytes;
    uint8_t* dst = static_cast<uint8_t*>(pg.tex.data);
    const uint8_t* sp = static_cast<const uint8_t*>(src->data);
    for (uint32_t ty = 0; ty < kAtlasH / 8u; ty++) {
        const uint32_t ss0 = AtlasSrcRow(ty * 8u, h, mirT);
        bool rowsWhole = (ss0 & 7u) == 0;
        for (uint32_t j = 1; j < 8u && rowsWhole; j++) {
            rowsWhole = AtlasSrcRow(ty * 8u + j, h, mirT) == ss0 + j;
        }
        for (uint32_t tx = 0; tx < 2u * w / 8u; tx++) {
            const uint32_t sx0 = AtlasSrcCol(tx * 8u, w, mirS);
            const bool colsWhole = (sx0 & 7u) == 0 && AtlasSrcCol(tx * 8u + 7u, w, mirS) == sx0 + 7u;
            uint8_t* d = dst + ((ty * (kAtlasW / 8u)) + (x0 / 8u) + tx) * tileBytes;
            if (rowsWhole && colsWhole) {
                memcpy(d, sp + (((ss0 >> 3) * (w >> 3)) + (sx0 >> 3)) * tileBytes, tileBytes);
                continue;
            }
            for (uint32_t j = 0; j < 8u; j++) {
                const uint32_t ss = AtlasSrcRow(ty * 8u + j, h, mirT);
                for (uint32_t i = 0; i < 8u; i++) {
                    const uint32_t sx = AtlasSrcCol(tx * 8u + i, w, mirS);
                    const uint32_t so = (((ss >> 3) * (w >> 3) + (sx >> 3)) * 64u + MortonOf(sx & 7u, ss & 7u)) * bytes;
                    memcpy(d + MortonOf(i, j) * bytes, sp + so, bytes);
                }
            }
        }
        gdx3ds_mesh_backend_flush_resident(dst + ((ty * (kAtlasW / 8u)) + (x0 / 8u)) * tileBytes, (2u * w / 8u) * tileBytes);
    }
}

/* The atlas draw material: the group's first member with the atlas on unit 0 (REPEAT both axes),
 * the atlas's texel-space rows, xoff.w = 32. Cold; rebuilt after a core state flush. */
__attribute__((noinline, cold)) bool AtlasMatBuild(AtlasPage& pg, const Mat& base) {
    Mat& a = pg.mat;
    if (&a != &base) {
        memcpy(&a, &base, sizeof(Mat));
    }
    a.atlasSlot1 = 0;
    a.atlasU = 32.0f;
    NrrStateKey k = base.skey;
    k.texObj[0] = &pg.tex;
    k.texCms[0] = 0;
    k.texCmt[0] = 0;
    a.skey = k;
    a.st = NrrStateGet(&k);
    if (a.st == nullptr) { /* the core's table is full (post-race screens): the page's own block memory */
        static void* sOwn[kAtlasPages] = {};
        void*& own = sOwn[&pg - sAtlas.page];
        own = own != nullptr ? own : ::operator new(NrrStateBytes(), std::nothrow);
        a.st = own != nullptr ? NrrStateBuildInto(&k, own) : nullptr;
    }
    if (a.st == nullptr) {
        pg.haveMat = false;
        return false;
    }
    a.stateGen = NrrStateGeneration();
    float* uva = a.unif + 4 * kRowUva0;
    uva[0] = 1.0f / (32.0f * (float)kAtlasW);
    uva[1] = 1.0f / (32.0f * (float)kAtlasH);
    uva[2] = 0.5f / (float)kAtlasW;
    uva[3] = 0.5f / (float)kAtlasH;
    pg.haveMat = true;
    sXfMat = nullptr;
    return true;
}

/* The walk alternates a tunnel's interior and shell (tile, tile + 1) op by op over the same rings: two
 * slots of one shapeType on two pages would cost a draw per op. When a new slot opens on another page
 * than an earlier slot of its shapeType, that slot is copied onto the new page too (room permitting)
 * and its materials move there; the old columns stay (rings may own them). Cold. */
__attribute__((noinline, cold)) void AtlasGatherPartners(int slot) {
    const AtlasSlot ns = sAtlas.slot[slot];
    AtlasPage& pg = sAtlas.page[ns.page];
    for (int q = 0; q < slot; q++) {
        const AtlasSlot old = sAtlas.slot[q];
        if (old.shapeType != ns.shapeType || old.page == ns.page) {
            continue;
        }
        NrrBkTex bt;
        if (sAtlas.nSlots >= kAtlasSlots || pg.xNext + 2u * old.w > kAtlasW || !gdx3ds_nrr_bk_tex(old.texId, &bt) ||
            bt.tex == nullptr || gdx3ds_nrr_bk_tex_serial(old.texId) != old.serial) {
            continue;
        }
        const int d = sAtlas.nSlots++;
        AtlasSlot& a = sAtlas.slot[d];
        a = old;
        a.page = ns.page;
        a.x0 = (uint16_t)pg.xNext;
        a.owner = 0;
        pg.xNext += 2u * old.w;
        AtlasFill(pg, static_cast<const C3D_Tex*>(bt.tex), a.x0, a.mirS != 0, a.mirT != 0);
        int movedMats = 0;
        for (int i = 0; i < kMats; i++) {
            if (sMats[i].atlasSlot1 == q + 1) {
                sMats[i].atlasSlot1 = (uint8_t)(d + 1);
                movedMats++;
            }
        }
        AtlasLog("[nrrtrk] atlas slot=%d page=%d copy of slot=%d (shape=%u type=%u, partner of slot=%d) x0=%u mats=%d used=%u/%u", d,
                 (int)a.page, q, (unsigned)(a.shapeType >> 6), (unsigned)(a.shapeType & 0x3Fu), slot, (unsigned)a.x0, movedMats,
                 (unsigned)pg.xNext, (unsigned)kAtlasW);
    }
}

__attribute__((noinline, cold)) bool AtlasBindView(const GdxTrkResView* view);
bool AtlasRingFits(const Gdx3dsVtxIn* v, uint32_t w);

/* An owner slot claims, once, every bake ring of its chunks' (shape, type): the ring's flag = x0 (one
 * flush for the span), before any atlas draw can read it; a ring whose U leaves the slot is refused. */
__attribute__((noinline, cold)) void AtlasClaimRings(int slot) {
    const AtlasSlot& a = sAtlas.slot[slot];
    if (!a.owner || sAtlas.view == nullptr || sAtlas.home == nullptr) {
        return;
    }
    const GdxTrkResSet& set = *sAtlas.view->set;
    Gdx3dsVtxIn* lin = const_cast<Gdx3dsVtxIn*>(sAtlas.view->linear);
    uint32_t lo = 0xFFFFFFFFu;
    uint32_t hi = 0;
    uint32_t claimed = 0;
    uint32_t refused = 0;
    for (uint32_t r = 0; r < sAtlas.nBase; r++) {
        if (sAtlas.home[r] != 0 || set.chunks[set.rings[r].chunk].info9 != a.shapeType) {
            continue;
        }
        if (!AtlasRingFits(&set.verts[r * 8u], a.w)) {
            sAtlas.home[r] = 0xFF;
            refused++;
            continue;
        }
        for (int v = 0; v < 8; v++) {
            lin[r * 8u + v].flag = (int16_t)a.x0;
        }
        sAtlas.home[r] = (uint8_t)(slot + 1);
        lo = r < lo ? r : lo;
        hi = r + 1u > hi ? r + 1u : hi;
        claimed++;
    }
    if (hi > lo) {
        gdx3ds_mesh_backend_flush_resident(&lin[lo * 8u], (hi - lo) * 8u * (uint32_t)sizeof(Gdx3dsVtxIn));
    }
    AtlasLog("[nrrtrk] atlas claim slot=%d rings=%u refused=%u span=%u", slot, claimed, refused, hi > lo ? hi - lo : 0u);
}

/* A resolved material into the atlas (cold): a slot for its texture, or refused (stays its own run). */
__attribute__((noinline, cold)) void AtlasAdmit(Mat& m, const GdxNrrTrkRun& run, const GdxTrkResView& view) {
    m.atlasSlot1 = 0;
    const NrrStateKey& k = m.skey;
    if (m.st == nullptr || k.tex[0] == NRR_TEX_NONE || k.tex[1] != NRR_TEX_NONE || k.texObj[0] != nullptr) {
        return;
    }
    if (sAtlas.gen != m.key.gen) {
        AtlasReset(m.key.gen);
    }
    if (sAtlas.view != &view && !AtlasBindView(&view)) {
        return;
    }
    NrrBkTex bt;
    NrrBkShader sh;
    if (!gdx3ds_nrr_bk_tex(k.tex[0], &bt) || bt.tex == nullptr || !gdx3ds_nrr_bk_translate(k.shaderId0, k.shaderId1, &sh)) {
        return;
    }
    const C3D_Tex* src = static_cast<const C3D_Tex*>(bt.tex);
    const uint32_t w = src->width;
    const uint32_t h = src->height;
    const bool mirS = (k.texCms[0] & 1u) != 0;
    const bool mirT = (k.texCmt[0] & 1u) != 0;
    const char* why = nullptr;
    if (bt.uOff != 0.0f || bt.vOff != 0.0f || bt.uScale != 1.0f || bt.vScale != 1.0f) {
        why = "view";
    } else if (!sh.usedTex[0] || sh.usedTex[1] || sh.clamp[0][0] || sh.clamp[0][1] || ((k.texCms[0] | k.texCmt[0]) & 2u) != 0) {
        why = "units/clamp";
    } else if (src->maxLevel != 0 || BppOf(src->fmt) < 8u || (sAtlas.page[0].texOk && src->fmt != sAtlas.page[0].tex.fmt)) {
        why = "levels/fmt";
    } else if (!IsPow2(w) || !IsPow2(h) || 2u * w > kAtlasW || (mirT ? 2u * h : h) > kAtlasH) {
        why = "size";
    } else if (!AtlasRowsOk(m, w, h)) {
        why = "rows";
    } else if (sAtlas.haveGroup && !AtlasSameGroup(sAtlas.group, m)) {
        why = "group";
    }
    int slot = -1;
    const uint32_t serial = gdx3ds_nrr_bk_tex_serial(k.tex[0]);
    for (int i = 0; why == nullptr && i < sAtlas.nSlots; i++) {
        const AtlasSlot& a = sAtlas.slot[i];
        if (a.texId == k.tex[0] && a.serial == serial && a.mirS == (mirS ? 1 : 0) && a.mirT == (mirT ? 1 : 0)) {
            slot = i;
        }
    }
    if (why == nullptr && slot < 0) {
        int pi = -1;
        for (int p = 0; p < sAtlas.nPages && pi < 0; p++) {
            pi = sAtlas.page[p].xNext + 2u * w <= kAtlasW ? p : -1;
        }
        if (pi < 0 && sAtlas.nPages < kAtlasPages) {
            pi = sAtlas.nPages++;
        }
        AtlasPage* pg = pi >= 0 ? &sAtlas.page[pi] : nullptr;
        if (sAtlas.nSlots >= kAtlasSlots || pg == nullptr) {
            why = "full";
        } else if (!pg->texOk) {
            pg->texOk = C3D_TexInit(&pg->tex, (u16)kAtlasW, (u16)kAtlasH, src->fmt);
            if (!pg->texOk) {
                why = "alloc";
                sAtlas.nPages = pi; /* not opened */
            } else {
                memset(pg->tex.data, 0, pg->tex.size);
                pg->tex.param = src->param;
                pg->tex.border = 0;
                pg->tex.lodParam = src->lodParam;
            }
        }
        if (why == nullptr) {
            if (!sAtlas.haveGroup) {
                memcpy(&sAtlas.group, &m, sizeof(Mat));
                sAtlas.haveGroup = true;
            }
            if (!pg->haveMat && !AtlasMatBuild(*pg, sAtlas.group)) {
                why = "state";
            }
        }
        if (why == nullptr) {
            slot = sAtlas.nSlots++;
            AtlasSlot& a = sAtlas.slot[slot];
            a.texId = k.tex[0];
            a.serial = serial;
            a.page = (uint8_t)pi;
            a.x0 = (uint16_t)pg->xNext;
            a.w = (uint16_t)w;
            a.shapeType = run.shapeType;
            a.mirS = mirS ? 1 : 0;
            a.mirT = mirT ? 1 : 0;
            a.owner = 1;
            for (int i = 0; i < slot; i++) {
                a.owner = sAtlas.slot[i].shapeType == run.shapeType ? 0 : a.owner;
            }
            pg->xNext += 2u * w;
            AtlasFill(*pg, src, a.x0, mirS, mirT);
            AtlasLog("[nrrtrk] atlas slot=%d page=%d gen=%u shape=%u type=%u tile=%u tex=%u %ux%u fmt=%d mir=%u%u x0=%u "
                     "owner=%u used=%u/%u",
                     slot, pi, (unsigned)m.key.gen, (unsigned)(run.shapeType >> 6), (unsigned)(run.shapeType & 0x3Fu),
                     (unsigned)run.tile, (unsigned)k.tex[0], (unsigned)w, (unsigned)h, (int)src->fmt, a.mirS, a.mirT,
                     (unsigned)a.x0, (unsigned)a.owner, (unsigned)pg->xNext, (unsigned)kAtlasW);
            AtlasClaimRings(slot);
            AtlasGatherPartners(slot);
        }
    }
    if (why != nullptr) {
        sAtlas.rejects++;
        AtlasLog("[nrrtrk] atlas refuse gen=%u shape=%u type=%u tile=%u tex=%u: %s", (unsigned)m.key.gen,
                 (unsigned)(run.shapeType >> 6), (unsigned)(run.shapeType & 0x3Fu), (unsigned)run.tile, (unsigned)k.tex[0],
                 why);
        return;
    }
    m.atlasSlot1 = (uint8_t)(slot + 1);
}

/* The view the atlas rings live in (the adopted bake): ownership table sized to it. */
__attribute__((noinline, cold)) bool AtlasBindView(const GdxTrkResView* view) {
    sAtlas.view = view;
    sAtlas.nBase = view->set->nRings;
    sAtlas.ringNext = view->set->nRings;
    if (sAtlas.homeCap < sAtlas.nBase) {
        free(sAtlas.home);
        sAtlas.home = static_cast<uint8_t*>(calloc(sAtlas.nBase, 1));
        sAtlas.homeCap = sAtlas.home != nullptr ? sAtlas.nBase : 0u;
    } else if (sAtlas.home != nullptr) {
        memset(sAtlas.home, 0, sAtlas.homeCap);
    }
    memset(sAtlas.hashKey, 0, sizeof(sAtlas.hashKey));
    sAtlas.dirtyLo = sAtlas.dirtyHi = 0;
    return sAtlas.home != nullptr;
}

/* The ring's U fits the slot unrolled: 0 <= U <= 32 (2w - 1) for all 8 vertices. */
bool AtlasRingFits(const Gdx3dsVtxIn* v, uint32_t w) {
    const int32_t hi = 32 * (2 * (int32_t)w - 1);
    for (int i = 0; i < 8; i++) {
        if (v[i].tc[0] < 0 || v[i].tc[0] > hi) {
            return false;
        }
    }
    return true;
}

/* Ring `ring` as slot `slot` draws it: the bake's ring (owned by the slot), or a copy. 0xFFFF = none.
 * Hot: the ownership byte, then the copy table; the first use of a (ring, slot) is cold. */
__attribute__((noinline, cold)) uint32_t AtlasRingCold(uint32_t ring, int slot, uint32_t i);
__attribute__((noinline)) uint32_t NrrTrkHotAtlasRing(uint32_t ring, int slot);

inline uint32_t AtlasRing(uint32_t ring, int slot) {
    if (ring < sAtlas.nBase && sAtlas.home[ring] == (uint8_t)(slot + 1)) {
        return ring;
    }
    return NrrTrkHotAtlasRing(ring, slot);
}

__attribute__((noinline)) uint32_t NrrTrkHotAtlasRing(uint32_t ring, int slot) {
    if (ring >= sAtlas.nBase) {
        return 0xFFFFu;
    }
    const uint32_t key = ((ring << 8) | (uint32_t)slot) + 1u;
    uint32_t i = (key * 0x9E3779B1u) >> 21; /* 11 bits */
    for (uint32_t probe = 0; probe < kAtlasHash; probe++, i = (i + 1u) & (kAtlasHash - 1u)) {
        if (sAtlas.hashKey[i] == key) {
            return sAtlas.hashVal[i];
        }
        if (sAtlas.hashKey[i] == 0) {
            break;
        }
    }
    return AtlasRingCold(ring, slot, i);
}

/* First use of (ring, slot): the slot takes the bake's ring when it owns its chunk's shapeType (the
 * ring's flag written once, before any atlas draw reads it), else a copy behind the bake's rings with
 * flag = x0. A ring whose U leaves the slot is refused (re-checked, cold, at each use). */
__attribute__((noinline, cold)) uint32_t AtlasRingCold(uint32_t ring, int slot, uint32_t i) {
    const AtlasSlot& a = sAtlas.slot[slot];
    const GdxTrkResSet& set = *sAtlas.view->set;
    Gdx3dsVtxIn* lin = const_cast<Gdx3dsVtxIn*>(sAtlas.view->linear);
    if (sAtlas.ringNext >= sAtlas.view->ringCap || sAtlas.hashKey[i] != 0 || !AtlasRingFits(&set.verts[ring * 8u], a.w)) {
        gdx3ds_nrr_trk_stats.atlasFail++;
        return 0xFFFFu; /* not stored: a ring that does not fit is re-checked (cold) each time */
    }
    const uint32_t c = sAtlas.ringNext++;
    for (int v = 0; v < 8; v++) {
        lin[c * 8u + v] = set.verts[ring * 8u + v];
        lin[c * 8u + v].flag = (int16_t)a.x0;
    }
    /* flushed with the hand-off's other copies (NrrTrkHotDrawHeld), before the draw is submitted */
    sAtlas.dirtyLo = sAtlas.dirtyHi > sAtlas.dirtyLo ? sAtlas.dirtyLo : c;
    sAtlas.dirtyHi = c + 1u;
    sAtlas.hashKey[i] = ((ring << 8) | (uint32_t)slot) + 1u;
    sAtlas.hashVal[i] = (uint16_t)c;
    sAtlas.copies++;
    gdx3ds_nrr_trk_stats.atlasCopies++;
    return c;
}

/* The copies the holds wrote, one flush (cold: only frames that made copies). */
__attribute__((noinline, cold)) void AtlasFlushCopies() {
    const Gdx3dsVtxIn* lin = sAtlas.view->linear;
    gdx3ds_mesh_backend_flush_resident(&lin[sAtlas.dirtyLo * 8u],
                                       (sAtlas.dirtyHi - sAtlas.dirtyLo) * 8u * (uint32_t)sizeof(Gdx3dsVtxIn));
    sAtlas.dirtyLo = sAtlas.dirtyHi = 0;
}

/* At a run's hold: its ops' rings as its atlas slot draws them (sOpRing); false = the run keeps its own
 * material (a ring outside the slot's U range, or no room for a copy). */
__attribute__((noinline)) int NrrTrkHotAtlasHold(const GdxTrkResView* view, const GdxNrrTrkFrame* f, const GdxNrrTrkRun& run,
                                           const Mat& m) {
    const int slot = (int)m.atlasSlot1 - 1;
    AtlasPage& pg = sAtlas.page[sAtlas.slot[slot].page];
    if (sAtlas.gen != f->gen || !pg.haveMat) {
        return 0;
    }
    if (sAtlas.view != view && !AtlasBindView(view)) {
        return 0;
    }
    if (pg.mat.stateGen != NrrStateGeneration() && !AtlasMatBuild(pg, pg.mat)) {
        return 0;
    }
    for (uint32_t k = run.opStart; k < run.opEnd; k++) {
        const GdxNrrTrkOp& op = f->ops[k];
        const uint32_t r0 = AtlasRing(op.ring[0], slot);
        const uint32_t r1 = AtlasRing(op.ring[1], slot);
        uint32_t rj = 0;
        if (gGdxTrkTemplate[op.tmpl].maxSlot >= 16) {
            rj = AtlasRing(op.join, slot);
        }
        if (r0 == 0xFFFFu || r1 == 0xFFFFu || rj == 0xFFFFu) {
            return 0;
        }
        sOpRing[k][0] = (uint16_t)r0;
        sOpRing[k][1] = (uint16_t)r1;
        sOpRing[k][2] = (uint16_t)rj;
    }
    return 1 + (int)sAtlas.slot[slot].page;
}

int CullOf(int xfCull) {
    if (xfCull == GDX3DS_CULL_BACK) {
        return NRR_CULL_BACK;
    }
    return xfCull == GDX3DS_CULL_FRONT ? NRR_CULL_FRONT : NRR_CULL_NONE;
}

/* Cold: the interpreter binds the material (the backend now holds it), the uniforms and aux are
 * built as a machine run builds them, and the backend's state is read back as a core block. */
__attribute__((noinline)) const Mat* MatResolve(GdxInterpSeam& I, const GdxTrkResView& view, uint32_t firstVertex,
                                                 const GdxNrrTrkRun& run, uint32_t gen) {
    const uint64_t t0 = svcGetSystemTick();
    Mat& m = sMats[sMatNext];
    sMatNext = (sMatNext + 1) % kMats;
    memset(&m, 0, sizeof(m));
    sXfMat = nullptr;
    const GdxSeamRegs& R = I.R();
    KeyOf(R, run, gen, &m.key);
    Gdx3dsLoadedVtx first;
    gdx3ds_vtx_transform(&sPass.vs, &view.set->verts[firstVertex], &first);
    Gdx3dsXfDraw xf;
    memset(&xf, 0, sizeof(xf));
    GdxSeamMatOutcome o;
    const int mode = 1 | GDXSEAM_MATREC_LIVE;
    gdx3ds_nrr_trk_stats.resolves++;
    sCold++;
    if (!gdx3ds_mesh_nrr_material(I, RecsFor(R), kRecWays, mode, sPass.vs, first, xf, m.aux, o, &m.info)) {
        gdx3ds_nrr_trk_stats.resolveTicks += svcGetSystemTick() - t0;
        return nullptr;
    }
    memcpy(m.unif, &xf.mvp[0][0], sizeof(m.unif));
    m.cullMode = xf.cullMode;
    MatInputs(R, m);
    NrrStateKey k;
    m.st = nullptr;
    m.texId[0] = m.texId[1] = NRR_TEX_NONE;
    if (sUseState && gdx3ds_nrr_bk_current_key(&k)) {
        k.cull = (uint8_t)CullOf(xf.cullMode);
        memcpy(m.scissor, k.scissor, sizeof(m.scissor));
        if (sMulti) {
            memset(k.scissor, 0, sizeof(k.scissor)); /* unused under keepVp: one block for every view */
            memcpy(m.scissor, kMultiScissor, sizeof(m.scissor));
        }
        if (sProbe != 0) {
            MatProbe(m, k);
        }
        const NrrState* st = MatState(m, k);
        const bool fogOk = st != nullptr && (!m.info.useFog || NrrStateFogStage(st) >= 0);
        if (fogOk) {
            m.st = st;
            m.stateGen = NrrStateGeneration();
            for (int u = 0; u < 2; u++) {
                m.texId[u] = k.tex[u];
                m.texSerial[u] = k.tex[u] != NRR_TEX_NONE ? gdx3ds_nrr_bk_tex_serial(k.tex[u]) : 0u;
            }
            m.skey = k;
            if (sCensus != 0) {
                CensusMat(m, k, run);
            }
            if (sAtlasOn) {
                AtlasAdmit(m, run, view);
            }
        } else {
            gdx3ds_nrr_trk_stats.noState++;
        }
    }
    gdx3ds_nrr_trk_stats.resolveTicks += svcGetSystemTick() - t0;
    return &m;
}

/* Cold: where each constant input's bytes live in the registers (BuildAux's InputFor cases). */
void InSrcOf(const GdxSeamRegs& R, uint8_t mux, const uint8_t** p, uint8_t* bcast) {
    *bcast = 0;
    *p = nullptr;
    switch (mux) {
        case GDXSEAM_CCMUX_PRIMITIVE: *p = &R.prim_color->r; break;
        case GDXSEAM_CCMUX_ENVIRONMENT: *p = &R.env_color->r; break;
        case GDXSEAM_CCMUX_PRIMITIVE_ALPHA: *p = &R.prim_color->a; *bcast = 1; break;
        case GDXSEAM_CCMUX_ENV_ALPHA: *p = &R.env_color->a; *bcast = 1; break;
        case GDXSEAM_CCMUX_PRIM_LOD_FRAC: *p = R.prim_lod_fraction; *bcast = 1; break;
        default: break;
    }
}

const uint8_t* AlphaSrcOf(const GdxSeamRegs& R, uint8_t mux) {
    switch (mux) {
        case GDXSEAM_CCMUX_PRIMITIVE: return &R.prim_color->a;
        case GDXSEAM_CCMUX_ENVIRONMENT: return &R.env_color->a;
        case GDXSEAM_ACMUX_PRIM_LOD_FRAC: return R.prim_lod_fraction;
        default: return nullptr;
    }
}

__attribute__((noinline)) void MatInputs(const GdxSeamRegs& R, Mat& m) {
    const int n = m.info.numInputs < 7 ? m.info.numInputs : 7;
    for (int j = 0; j < n; j++) {
        InSrcOf(R, m.info.inputMap[0][j], &m.in[j].p, &m.in[j].bcast);
        m.in[j].alpha = m.info.useAlpha;
        m.in[j].pa = m.info.useAlpha ? AlphaSrcOf(R, m.info.inputMap[1][j]) : nullptr;
    }
}

/* The per-frame constants of a material from the registers (the inputs' bytes as MatInputs found
 * them; the fog colour; the fog alpha), into `in` / `fog` / `*fogA`. */
__attribute__((noinline)) void NrrTrkHotConsts(const GdxSeamRegs& R, const Mat& m, float (*in)[4], float* fog, float* fogA) {
    static const uint8_t kZero[4] = { 0, 0, 0, 0 };
    const float k = 1.0f / 255.0f; /* byte -> unit: NrrCoreHotPack rounds back to the same byte */
    const int n = m.info.numInputs < 7 ? m.info.numInputs : 7;
    for (int j = 0; j < n; j++) {
        const InSrc& is = m.in[j];
        const uint8_t* p = is.p != nullptr ? is.p : kZero;
        const int step = is.bcast ? 0 : 1;
        float* o = in[j];
        o[0] = p[0] * k;
        o[1] = p[step] * k;
        o[2] = p[2 * step] * k;
        o[3] = !is.alpha ? 1.0f : (is.pa != nullptr ? is.pa[0] * k : 0.0f);
    }
    if (m.info.useFog) {
        const GdxSeamRgba& f = m.info.useBlendColor ? *R.blend_color : *R.fog_color;
        fog[0] = f.r * k;
        fog[1] = f.g * k;
        fog[2] = f.b * k;
    } else {
        fog[0] = fog[1] = fog[2] = 0.0f;
    }
    *fogA = R.fog_color->a / 255.0f;
}

/* sCur's constants into the TexEnv constants and the fog stage. */
void NrrTrkHotSetConsts(NrrCtx* c, const Mat& m) {
    const int n = m.info.numInputs < 7 ? m.info.numInputs : 7;
    NrrSetInputs(c, m.st, sCur.in, n);
    if (m.info.useFog) {
        NrrSetFog(c, m.st, sCur.fog);
    }
}

void NrrTrkHotInputs(NrrCtx* c, const GdxSeamRegs& R, const Mat& m) {
    NrrTrkHotConsts(R, m, sCur.in, sCur.fog, &sCur.fogA);
    NrrTrkHotSetConsts(c, m);
}

/* The 28-vec4 uniform block of one draw: the material's template, the pass's matrix, eye offset
 * and fog line, and the colour/alpha constants this frame's registers give. */
void NrrTrkHotUniformRows(const Mat& m, float* u);

void NrrTrkHotUniforms(const Mat& m, float* u) {
    memcpy(u, m.unif, sizeof(m.unif));
    NrrTrkHotUniformRows(m, u);
}

/* The rows of the block a draw sets over the template (pass rows, col). */
void NrrTrkHotUniformRows(const Mat& m, float* u) {
    Gdx3dsXfDraw* xf = reinterpret_cast<Gdx3dsXfDraw*>(reinterpret_cast<char*>(u) - offsetof(Gdx3dsXfDraw, mvp));
    memcpy(xf->mvp, sPass.mvpT, sizeof(sPass.mvpT));
    const float fogOn = sPass.vs.fog ? 1.0f : 0.0f;
    xf->fog[0] = (float)sPass.vs.fogMul;
    xf->fog[1] = (float)sPass.vs.fogOffset;
    xf->fog[2] = fogOn;
    xf->fog[3] = 1.0f - fogOn;
    if (m.info.colConst && m.info.ci >= 0 && m.info.ci < 7) {
        xf->col[0] = sCur.in[m.info.ci][0];
        xf->col[1] = sCur.in[m.info.ci][1];
        xf->col[2] = sCur.in[m.info.ci][2];
    }
    if (m.info.fogFactorConst) {
        xf->col[3] = sCur.fogA;
    } else if (m.info.alphaConst && m.info.ai >= 0 && m.info.ai < 7) {
        xf->col[3] = sCur.in[m.info.ai][3];
    }
    xf->off[0] = -(float)sPass.origin[0];
    xf->off[1] = -(float)sPass.origin[1];
    xf->off[2] = -(float)sPass.origin[2];
    xf->off[3] = m.atlasU;
}

/* n60draw (nrr_draw bit 2): the rows a trk block may change between two draws of one pass. The
 * pass rows (mvp, fog, off) come from sPass; every other row is the material's template, except
 * col, which the frame's constants rewrite. */
constexpr uint32_t kRowsPass = 0xFu | (1u << 6) | (1u << 27);
constexpr uint32_t kRowCol = 1u << 23;
constexpr uint32_t kRowsAll = (1u << 28) - 1u;

/* The block of NrrTrkHotUniforms written into the material's own template (the rows it overwrites
 * are the ones every draw overwrites), so no 448-byte copy per draw; returns the rows that may
 * differ from the previous block drawn. */
__attribute__((noinline)) uint32_t NrrTrkHotUniformsInPlace(const Mat& m) {
    Mat& mm = const_cast<Mat&>(m);
    NrrTrkHotUniformRows(m, mm.unif);
    uint32_t rows = kRowsAll;
    if (sXfPass == sPass.serial) {
        rows = sXfMat == &m ? kRowCol : (kRowsAll & ~kRowsPass);
        /* n60atlas: xoff.w is the material's (32 on an atlas page, 0 elsewhere), not the pass's */
        rows |= sXfAtlasU != m.atlasU ? 1u << 27 : 0u;
    }
    sXfMat = &m;
    sXfPass = sPass.serial;
    sXfAtlasU = m.atlasU;
    return rows;
}

/* ---- the draw ---------------------------------------------------------------------------------- */

/* The run's indices live in the frame's linear pool (NrrRingAlloc): one allocation per run, drawn
 * in segments at material boundaries. */
struct IdxRing {
    uint16_t* base;
    uint32_t cap;
    uint32_t start; /* first index of the open segment */
    uint32_t n;     /* indices written */
};
IdxRing sRing = {};

/* The fog line a fog material's backend draw is baked under (the mesh path's note before its draw). */
void BackendFogLine(const GdxSeamRegs& R, const Mat& m) {
    if (!m.info.useFog || &gdx3ds_fog_note_params == nullptr) {
        return;
    }
    if (m.info.useBlendColor) {
        gdx3ds_fog_note_params(0, 0, 0);
    } else {
        gdx3ds_fog_note_params(*R.fog_mul, *R.fog_offset, 1);
    }
}

/* The backend path for a material the core cannot express (and the interpreter-bound cold draw). */
__attribute__((noinline)) void BackendDraw(NrrCtx* c, GdxInterpSeam& I, const Mat& m, const GdxTrkResView& view) {
    NrrEnd(c); /* citro3d back to what the backend believes it holds */
    Gdx3dsXfDraw xf;
    memset(&xf, 0, sizeof(xf));
    memcpy(&xf.mvp[0][0], m.unif, sizeof(m.unif));
    NrrTrkHotUniforms(m, &xf.mvp[0][0]);
    xf.verts = view.linear;
    xf.nVerts = view.nVerts;
    xf.clsW = 1.5f;
    xf.cullMode = m.cullMode; /* n60retry: was left 0 (no culling): backfaces drew over the road */
    xf.stateElide = 1;
    I.ApplyDrawConstants();
    BackendFogLine(I.R(), m);
    gdx3ds_mesh_backend_xf_draw(sRing.base + sRing.start, sRing.n - sRing.start, &m.aux, &xf);
    NrrBegin(c);
    sCur.m = nullptr;
}

void NrrTrkHotFlush(NrrCtx* c, GdxInterpSeam& I, const GdxTrkResView& view) {
    const uint32_t n = sRing.n - sRing.start;
    if (n == 0 || sCur.m == nullptr) {
        return;
    }
    if (sCur.m->st == nullptr) {
        BackendDraw(c, I, *sCur.m, view);
    } else if (sXfRows) {
        const uint32_t rows = NrrTrkHotUniformsInPlace(*sCur.m);
        NrrDrawXfRows(c, view.linear, 16, sRing.base + sRing.start, n, sCur.m->unif, rows);
    } else {
        alignas(8) float u[kXfFloats];
        NrrTrkHotUniforms(*sCur.m, u);
        NrrDrawXf(c, view.linear, 16, sRing.base + sRing.start, n, u);
    }
    gdx3ds_nrr_trk_stats.draws++;
    sRing.start = sRing.n;
}

/* The material in force: found (hot) or resolved (cold, outside the hand-off), then applied. */
__attribute__((noinline)) bool NrrTrkHotMaterial(NrrCtx* c, GdxInterpSeam& I, const GdxTrkResView& view, uint32_t firstVertex,
                       const GdxNrrTrkRun& run, uint32_t gen) {
    const Mat* m = NrrTrkHotMatFind(I.R(), run, gen);
    if (m == nullptr) {
        NrrEnd(c); /* the interpreter's bind writes citro3d through the backend */
        m = MatResolve(I, view, firstVertex, run, gen);
        NrrBegin(c);
        if (m == nullptr) {
            return false;
        }
    }
    sCur.m = m;
    if (sCensus != 0) {
        CensusRun(m);
    }
    sCur.fogA = I.R().fog_color->a / 255.0f; /* the backend path's uniforms read it too */
    if (m->st != nullptr) {
        NrrStateApply(c, m->st);
        NrrTrkHotInputs(c, I.R(), *m);
    }
    return true;
}

__attribute__((noinline)) void NrrTrkHotEvent(NrrCtx* c, GdxInterpSeam& I, const GdxTrkResView& view, const GdxTrkCombineEvent& e) {
    NrrTrkHotFlush(c, I, view);
    I.SetCombineRaw(e.w0, e.w1);
    sCur.m = nullptr;
    gdx3ds_nrr_trk_stats.events++;
}

/* Triangles [from, to) of template t over the op's two resident rings, into the run's indices. */
__attribute__((noinline)) void NrrTrkHotAppend(const GdxTrkTemplate& t, int from, int to, const uint32_t base[3]) {
    const uint32_t n = 3u * (uint32_t)(to - from);
    uint16_t* d = sRing.base + sRing.n;
    const uint8_t* idx = &t.idx[from * 3];
    for (uint32_t j = 0; j < n; j++) {
        d[j] = (uint16_t)(base[idx[j] >> 3] + (idx[j] & 7u));
    }
    sRing.n += n;
    gdx3ds_nrr_trk_stats.tris += (uint32_t)(to - from);
}

/* ---- n60draw: pair-packed index append ([native] nrr_draw bit 8) --------------------------------
 *
 * A template index is slot = sel * 8 + off (sel 0/1 = the op's two rings, 2 = its join ring) and the
 * draw index is base[sel] + off with base[sel] = ring * 8 (< 65536: a resident uint16 index). Two
 * indices are one 32-bit word, so per pair: the word of the two bases (one of 9 combinations, built
 * per op) plus the pair's two offsets (precomputed per template). Phase = the destination's
 * alignment (the open draw's index count parity): phase 1 writes one index alone first. Same
 * indices in the same order as NrrTrkHotAppend. */
constexpr int kPairMax = GDXTRK_MAX_TRIS * 3 / 2;

struct PairTab {
    uint8_t nPairs[2];
    uint8_t lead[2];  /* phase p: slot written alone before the pairs (0xFF none) */
    uint8_t tail[2];  /* ...after them */
    uint8_t combo[2][kPairMax]; /* sel_a * 3 + sel_b */
    uint32_t off[2][kPairMax];  /* off_a | off_b << 16 */
};
PairTab sPair[GDXTRK_TEMPLATE_COUNT];
bool sPairBuilt = false;

typedef uint32_t __attribute__((may_alias, aligned(4))) PairWord;

__attribute__((noinline, cold)) void PairBuild() {
    for (int t = 0; t < GDXTRK_TEMPLATE_COUNT; t++) {
        const GdxTrkTemplate& tm = gGdxTrkTemplate[t];
        const int n = 3 * tm.ntris;
        PairTab& pt = sPair[t];
        for (int ph = 0; ph < 2; ph++) {
            int i = 0;
            pt.lead[ph] = 0xFF;
            pt.tail[ph] = 0xFF;
            if (ph == 1 && n > 0) {
                pt.lead[ph] = tm.idx[0];
                i = 1;
            }
            int k = 0;
            for (; i + 1 < n; i += 2, k++) {
                const uint8_t a = tm.idx[i];
                const uint8_t b = tm.idx[i + 1];
                pt.combo[ph][k] = (uint8_t)((a >> 3) * 3 + (b >> 3));
                pt.off[ph][k] = (uint32_t)(a & 7u) | ((uint32_t)(b & 7u) << 16);
            }
            pt.nPairs[ph] = (uint8_t)k;
            if (i < n) {
                pt.tail[ph] = tm.idx[i];
            }
        }
    }
    sPairBuilt = true;
}

void PairEnsure() {
    if (!sPairBuilt) {
        PairBuild();
    }
}

/* A whole template (from 0 to ntris) into the open draw's indices, pair-packed. */
__attribute__((noinline)) void NrrTrkHotAppendPairs(int tmpl, const uint32_t base[3]) {
    const GdxTrkTemplate& t = gGdxTrkTemplate[tmpl];
    const PairTab& pt = sPair[tmpl];
    uint16_t* d = sRing.base + sRing.n;
    const int ph = (int)(sRing.n & 1u);
    const uint32_t b0 = base[0];
    const uint32_t b1 = base[1];
    uint32_t bp[9];
    bp[0] = b0 | (b0 << 16);
    bp[1] = b0 | (b1 << 16);
    bp[3] = b1 | (b0 << 16);
    bp[4] = b1 | (b1 << 16);
    if (t.maxSlot >= 16) {
        const uint32_t b2 = base[2];
        bp[2] = b0 | (b2 << 16);
        bp[5] = b1 | (b2 << 16);
        bp[6] = b2 | (b0 << 16);
        bp[7] = b2 | (b1 << 16);
        bp[8] = b2 | (b2 << 16);
    }
    if (pt.lead[ph] != 0xFF) {
        *d++ = (uint16_t)(base[pt.lead[ph] >> 3] + (pt.lead[ph] & 7u));
    }
    PairWord* w = reinterpret_cast<PairWord*>(d);
    const uint8_t* cb = pt.combo[ph];
    const uint32_t* of = pt.off[ph];
    const int np = pt.nPairs[ph];
    for (int k = 0; k < np; k++) {
        w[k] = bp[cb[k]] + of[k];
    }
    if (pt.tail[ph] != 0xFF) {
        d[2 * np] = (uint16_t)(base[pt.tail[ph] >> 3] + (pt.tail[ph] & 7u));
    }
    sRing.n += 3u * t.ntris;
    gdx3ds_nrr_trk_stats.tris += t.ntris;
}

__attribute__((noinline)) bool NrrTrkHotOp(NrrCtx* c, GdxInterpSeam& I, const GdxTrkResView& view, const GdxNrrTrkOp& op,
                 const GdxNrrTrkRun& run, uint32_t gen) {
    const GdxTrkResSet& set = *view.set;
    if (op.tmpl >= GDXTRK_TEMPLATE_COUNT || op.ring[0] >= set.nRings || op.ring[1] >= set.nRings) {
        return false;
    }
    const GdxTrkTemplate& t = gGdxTrkTemplate[op.tmpl];
    /* slots 16-20: a join template's join vertices (n60trk2; NO_RING >= nRings); no other template
       reads base[2] */
    if (t.maxSlot >= 16 && op.join >= set.nRings) {
        return false;
    }
    const uint32_t base[3] = { (uint32_t)op.ring[0] * 8u, (uint32_t)op.ring[1] * 8u, (uint32_t)op.join * 8u };
    if (sDiag) {
        DiagOp(set, op, t, base);
    }
    int ev = 0;
    int i = 0;
    while (i < t.ntris) {
        while (ev < t.nevents && t.events[ev].beforeTri == i) {
            NrrTrkHotEvent(c, I, view, t.events[ev++]);
        }
        const int end = ev < t.nevents ? t.events[ev].beforeTri : t.ntris;
        if (end > i) {
            if (sCur.m == nullptr &&
                !NrrTrkHotMaterial(c, I, view, base[t.idx[i * 3] >> 3] + (t.idx[i * 3] & 7u), run, gen)) {
                return false;
            }
            if (sPairAppend && i == 0 && end == t.ntris) {
                NrrTrkHotAppendPairs(op.tmpl, base);
            } else {
                NrrTrkHotAppend(t, i, end, base);
            }
        }
        i = end > i ? end : i + 1;
    }
    while (ev < t.nevents) {
        NrrTrkHotEvent(c, I, view, t.events[ev++]);
    }
    gdx3ds_nrr_trk_stats.ops++;
    return true;
}

__attribute__((noinline)) void InitLog(GdxInterpSeam& I) {
    if (sInitLogged) {
        return;
    }
    sInitLogged = true;
    LatchLevers();
    Log("[nrrtrk] draw init reject_variant=%d xf_ready=%d state_blocks=%d f24=%d merge=%d atlas=%d recs=%dx%d (%u bytes) "
        "mats=%d (%u bytes) held=%d (%u bytes)",
        I.RejectVariant() ? 1 : 0, gdx3ds_mesh_backend_xf_ready(), sUseState ? 1 : 0, sDiag ? 1 : 0,
        sMerge ? 1 : 0, sAtlasOn ? 1 : 0,
        kRecBuckets, kRecWays, (unsigned)sizeof(sRecs), kMats, (unsigned)sizeof(sMats), kHeldMax, (unsigned)sizeof(sHeld));
}

/* The frame's runs are drawn in marker order: the next one for this (core frame, tick). */
struct RunCursor {
    const NrrFrame* nf;
    uint32_t tick;
    uint32_t next;
    const GdxNrrTrkFrame* f; /* niview: each view of a multi-view race has its own snapshot */
};
RunCursor sCursor = { nullptr, 0, 0, nullptr };

__attribute__((noinline)) const GdxNrrTrkRun* NrrTrkHotNextRun(const NrrFrame* nf, const GdxNrrTrkFrame* f) {
    if (sCursor.nf != nf || sCursor.tick != NrrFrameTick(nf) || sCursor.f != f) {
        sCursor.nf = nf;
        sCursor.tick = NrrFrameTick(nf);
        sCursor.next = 0;
        sCursor.f = f;
    }
    if (sCursor.next >= f->nRuns) {
        return nullptr;
    }
    return &f->runs[sCursor.next++];
}

/* A new course: the records and materials name the old one's textures. */
__attribute__((noinline)) void NewCourse(uint32_t gen) {
    sRecGen = gen;
    memset(sRecs, 0, sizeof(sRecs));
    memset(sMats, 0, sizeof(sMats)); /* the own block memory stays allocated for the new Mats */
    sXfMat = nullptr;
}

/* The key form of a scissor rect (gfx_citro3d_nrr.inc NrrScissorKey: all 0 = the full window). */
void ScissorKeyOf(const int32_t r[4], int16_t out[4]) {
    if (r[0] == 0 && r[1] == 0 && r[2] == 400 && r[3] == 240) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = (int16_t)r[i];
    }
}

/* n60retry: the walk's viewport / scissor (Camera_Draw, in the sky's head) reaches the backend only
 * at the next interpreter triangle; with the sky on the NRR there is none before the track, so a
 * camera viewport transition (the post-race name entry) drew the track under the previous
 * frame's. A pending viewport goes to the backend with the hand-off closed (the backend changes
 * citro3d; the projection map folds it); the cached blocks are checked against the registers'
 * scissor, the one the stock triangles of this run would be clipped to. */
__attribute__((noinline)) void SyncViewportScissor(NrrCtx* c, GdxInterpSeam& I) {
    if (I.ViewportPending()) {
        NrrEnd(c);
        I.ApplyViewportScissor();
        NrrBegin(c);
        gdx3ds_nrr_trk_stats.vpSyncs++;
    }
    int32_t r[4];
    if (sMulti) {
        memcpy(sRunScissor, kMultiScissor, sizeof(sRunScissor));
    } else if (I.RegisterScissor(r)) {
        ScissorKeyOf(r, sRunScissor);
    } else {
        gdx3ds_nrr_bk_scissor_key(sRunScissor);
    }
}

/* Indices the run will write (its templates' triangles), for one pool allocation. */
__attribute__((noinline)) uint32_t NrrTrkHotRunIdx(const GdxNrrTrkFrame* f, const GdxNrrTrkRun& run) {
    uint32_t n = 0;
    for (uint32_t k = run.opStart; k < run.opEnd && k < f->nOps; k++) {
        const uint8_t t = f->ops[k].tmpl;
        n += t < GDXTRK_TEMPLATE_COUNT ? 3u * gGdxTrkTemplate[t].ntris : 0u;
    }
    return n;
}

/* Drop what a frame left held (its last marker never came: counted, must stay 0). */
__attribute__((noinline)) void HeldDrop() {
    gdx3ds_nrr_trk_stats.heldLost += (uint32_t)sHeld.n;
    sHeld.n = 0;
    sHeld.nIdx = 0;
    sHeld.closeNow = false;
    gdx3ds_nrr_set_pending(NRR_CAT_TRK, 0);
}

/* Held runs belong to one (core frame, tick). */
inline bool HeldOf(const NrrFrame* nf) {
    return sHeld.nf == nf && sHeld.tick == NrrFrameTick(nf);
}

/* Can `run` be held, and if so its record: the checks the hand-off path would make, read now. */
__attribute__((noinline)) bool NrrTrkHotHold(GdxInterpSeam& I, const GdxNrrTrkFrame* f, const GdxNrrTrkRun& run,
                                             HeldRun& h) {
    const GdxTrkResView* view = GdxTrkRes::Acquire(f->frame);
    if (view == nullptr || view->set->generation != f->gen || sRecGen != f->gen || !BeginPass(I, f) ||
        I.ViewportPending()) {
        return false;
    }
    const GdxTrkResSet& set = *view->set;
    uint32_t n = 0;
    for (uint32_t k = run.opStart; k < run.opEnd; k++) {
        if (k >= f->nOps) {
            return false;
        }
        const GdxNrrTrkOp& op = f->ops[k];
        if (op.tmpl >= GDXTRK_TEMPLATE_COUNT || op.ring[0] >= set.nRings || op.ring[1] >= set.nRings) {
            return false;
        }
        const GdxTrkTemplate& t = gGdxTrkTemplate[op.tmpl];
        if (t.nevents != 0 || (t.maxSlot >= 16 && op.join >= set.nRings)) {
            return false;
        }
        n += 3u * t.ntris;
    }
    int32_t r[4];
    if (I.RegisterScissor(r)) {
        ScissorKeyOf(r, sRunScissor);
    } else {
        gdx3ds_nrr_bk_scissor_key(sRunScissor);
    }
    const Mat* m = NrrTrkHotMatFind(I.R(), run, f->gen);
    if (m == nullptr) {
        return false;
    }
    h.m = m;
    h.run = &run;
    h.nIdx = n;
    h.atlas = m->atlasSlot1 != 0 ? NrrTrkHotAtlasHold(view, f, run, *m) : 0;
    NrrTrkHotConsts(I.R(), *m, h.in, h.fog, &h.fogA);
    return true;
}

/* The core's peek for NRR_CAT_TRK: 1 = this run is held (no hand-off). */
int NrrTrkHotPeek(void* seam, int view, const NrrFrame* nf) {
    LatchLevers();
    if (!sMerge || view != 0) {
        return 0;
    }
    GdxInterpSeam& I = *static_cast<GdxInterpSeam*>(seam);
    const GdxNrrTrkFrame* const* root = static_cast<const GdxNrrTrkFrame* const*>(NrrSnapGet(nf, NRR_CAT_TRK, view));
    if (root == nullptr || *root == nullptr || (*root)->magic != GDX_NRR_TRK_MAGIC) {
        return 0;
    }
    const GdxNrrTrkFrame* f = *root;
    if (f->views > 1) {
        return 0; /* niview: a multi-view race's runs draw at their own markers (no merge) */
    }
    if (sHeld.n != 0 && (!HeldOf(nf) || sHeld.f != f)) {
        HeldDrop();
    }
    if (sCursor.nf != nf || sCursor.tick != NrrFrameTick(nf) || sCursor.f != f) {
        sCursor.nf = nf;
        sCursor.tick = NrrFrameTick(nf);
        sCursor.next = 0;
        sCursor.f = f;
    }
    if (f->fbGroups != 0 || sCursor.next >= f->nRuns || sHeld.n >= kHeldMax) {
        return 0;
    }
    const GdxNrrTrkRun& run = f->runs[sCursor.next];
    if (!NrrTrkHotHold(I, f, run, sHeld.r[sHeld.n])) {
        return 0;
    }
    sCursor.next++;
    sHeld.nf = nf;
    sHeld.tick = NrrFrameTick(nf);
    sHeld.f = f;
    sHeld.n++;
    sHeld.nIdx += sHeld.r[sHeld.n - 1].nIdx;
    gdx3ds_nrr_trk_stats.held++; /* the peek is not in `ticks` (no SVC per held run): [nrr] trk us has it */
    if (sCursor.next >= f->nRuns) {
        sHeld.closeNow = true; /* the frame's last run: this marker's hand-off draws them all */
        return 0;
    }
    gdx3ds_nrr_set_pending(NRR_CAT_TRK, 1);
    return 1;
}

/* Every op of a run into the open draw's indices (an atlas run: its slot's rings, sOpRing). */
void NrrTrkHotAppendRun(const GdxNrrTrkFrame* f, const GdxNrrTrkRun& run, bool atlas) {
    for (uint32_t k = run.opStart; k < run.opEnd; k++) {
        const GdxNrrTrkOp& op = f->ops[k];
        const GdxTrkTemplate& t = gGdxTrkTemplate[op.tmpl];
        const uint32_t base[3] = { (uint32_t)(atlas ? sOpRing[k][0] : op.ring[0]) * 8u,
                                   (uint32_t)(atlas ? sOpRing[k][1] : op.ring[1]) * 8u,
                                   (uint32_t)(atlas ? sOpRing[k][2] : op.join) * 8u };
        if (sPairAppend) {
            NrrTrkHotAppendPairs(op.tmpl, base);
        } else {
            NrrTrkHotAppend(t, 0, t.ntris, base);
        }
        gdx3ds_nrr_trk_stats.ops++;
    }
    gdx3ds_nrr_trk_stats.runs++;
}

/* The material a held run draws with: the atlas material for an atlas run, else its own. */
inline const Mat* DrawMatOf(const HeldRun& h) {
    return h.atlas ? &sAtlas.page[h.atlas - 1].mat : h.m;
}

/* A held run's constants are the current ones (same material, same bytes): it joins the open draw. */
bool SameAsCur(const HeldRun& h) {
    const int n = h.m->info.numInputs < 7 ? h.m->info.numInputs : 7;
    return sCur.m == DrawMatOf(h) && sCur.fogA == h.fogA && memcmp(sCur.fog, h.fog, sizeof(h.fog)) == 0 &&
           memcmp(sCur.in, h.in, sizeof(float) * 4u * (uint32_t)n) == 0;
}

/* Inside a hand-off: the held runs in marker order, one draw per material/constant change. */
__attribute__((noinline)) int NrrTrkHotDrawHeld(NrrCtx* c, GdxInterpSeam& I) {
    const GdxNrrTrkFrame* f = sHeld.f;
    const GdxTrkResView* view = GdxTrkRes::Acquire(f->frame);
    if (view == nullptr || view->set->generation != f->gen) {
        gdx3ds_nrr_trk_stats.genMiss++;
        HeldDrop();
        return 2;
    }
    sRing.base = static_cast<uint16_t*>(NrrRingAlloc(c, sHeld.nIdx * 2u + 16u));
    if (sRing.base == nullptr) {
        gdx3ds_nrr_trk_stats.badRuns++;
        HeldDrop();
        return 5;
    }
    sRing.cap = sHeld.nIdx;
    sRing.start = 0;
    sRing.n = 0;
    sCur.m = nullptr;
    if (sAtlas.dirtyHi != 0) {
        AtlasFlushCopies();
    }
    for (int i = 0; i < sHeld.n; i++) {
        const HeldRun& h = sHeld.r[i];
        if (sCensus != 0) {
            CensusRun(h.m);
        }
        gdx3ds_nrr_trk_stats.atlasRuns += h.atlas != 0 ? 1u : 0u;
        if (SameAsCur(h)) {
            gdx3ds_nrr_trk_stats.heldJoined++;
            gdx3ds_nrr_trk_stats.atlasJoined += h.atlas != 0 ? 1u : 0u;
        } else {
            NrrTrkHotFlush(c, I, *view);
            const Mat& hm = *h.m;
            if ((hm.texId[0] != NRR_TEX_NONE && gdx3ds_nrr_bk_tex_serial(hm.texId[0]) != hm.texSerial[0]) ||
                (hm.texId[1] != NRR_TEX_NONE && gdx3ds_nrr_bk_tex_serial(hm.texId[1]) != hm.texSerial[1])) {
                gdx3ds_nrr_trk_stats.heldStale++; /* a texture re-uploaded since the hold: must stay 0 */
            }
            const Mat& m = *DrawMatOf(h);
            sCur.m = &m;
            memcpy(sCur.in, h.in, sizeof(sCur.in));
            memcpy(sCur.fog, h.fog, sizeof(sCur.fog));
            sCur.fogA = h.fogA;
            NrrStateApply(c, m.st);
            NrrTrkHotSetConsts(c, m);
        }
        NrrTrkHotAppendRun(f, *h.run, h.atlas != 0);
    }
    NrrTrkHotFlush(c, I, *view);
    sCur.m = nullptr; /* a run drawn after these resolves its own material */
    sHeld.n = 0;
    sHeld.nIdx = 0;
    sHeld.closeNow = false;
    gdx3ds_nrr_set_pending(NRR_CAT_TRK, 0);
    return 0;
}

__attribute__((noinline)) int NrrTrkHotRun(NrrCtx* c, GdxInterpSeam& I, const GdxNrrTrkFrame* f, const GdxNrrTrkRun& run) {
    const GdxTrkResView* view = GdxTrkRes::Acquire(f->frame);
    if (view == nullptr || view->set->generation != f->gen) {
        gdx3ds_nrr_trk_stats.genMiss++;
        return 2;
    }
    if (sRecGen != f->gen) {
        NewCourse(f->gen);
    }
    if (!BeginPass(I, f)) {
        gdx3ds_nrr_trk_stats.badRuns++;
        return 3;
    }
    if (sHeld.n != 0) {
        NrrTrkHotDrawHeld(c, I); /* the held runs come first: marker order */
    }
    sMulti = f->views > 1;
    if (sMulti) {
        c->keepVp = 1; /* niview: this view's viewport/scissor (before the first state apply) */
        gdx3ds_nrr_note_view(c->view);
    }
    SyncViewportScissor(c, I);
    const uint32_t need = NrrTrkHotRunIdx(f, run);
    sRing.base = static_cast<uint16_t*>(NrrRingAlloc(c, need * 2u + 16u));
    if (sRing.base == nullptr) {
        gdx3ds_nrr_trk_stats.badRuns++;
        return 5;
    }
    sRing.cap = need;
    sRing.start = 0;
    sRing.n = 0;
    sCur.m = nullptr; /* the walk's texture-state GBI ran since the last run */
    int rc = 0;
    for (uint32_t k = run.opStart; k < run.opEnd && k < f->nOps; k++) {
        if (!NrrTrkHotOp(c, I, *view, f->ops[k], run, f->gen)) {
            gdx3ds_nrr_trk_stats.badRuns++;
            rc = 4;
            break;
        }
    }
    NrrTrkHotFlush(c, I, *view);
    gdx3ds_nrr_trk_stats.runs++;
    return rc;
}

/* The NRR core's draw entry for NRR_CAT_TRK: one material run per 0x63 marker. */
int NrrTrkHotDraw(NrrCtx* c, int view, const NrrFrame* nf) {
    GdxInterpSeam& I = *static_cast<GdxInterpSeam*>(c->seam);
    if (sHeld.n != 0 && (view == NRR_VIEW_DRAIN || sHeld.closeNow)) {
        const uint64_t t0 = svcGetSystemTick();
        InitLog(I);
        int rc = 0;
        if (view == NRR_VIEW_DRAIN) {
            /* the core drains only within the task that held them (task end clears its pending) */
            gdx3ds_nrr_trk_stats.heldDrains++;
            rc = NrrTrkHotDrawHeld(c, I);
        } else if (!HeldOf(nf)) {
            HeldDrop();
        } else {
            rc = NrrTrkHotDrawHeld(c, I);
        }
        gdx3ds_nrr_trk_stats.ticks += svcGetSystemTick() - t0;
        return rc;
    }
    if (view == NRR_VIEW_DRAIN) {
        return 0;
    }
    const GdxNrrTrkFrame* const* root = static_cast<const GdxNrrTrkFrame* const*>(NrrSnapGet(nf, NRR_CAT_TRK, view));
    if (root == nullptr || *root == nullptr || (*root)->magic != GDX_NRR_TRK_MAGIC) {
        gdx3ds_nrr_trk_stats.badRuns++;
        return 1;
    }
    const GdxNrrTrkRun* run = NrrTrkHotNextRun(nf, *root);
    if (run == nullptr) {
        gdx3ds_nrr_trk_stats.badRuns++;
        return 1;
    }
    const uint64_t t0 = svcGetSystemTick();
    InitLog(I);
    const int rc = NrrTrkHotRun(c, I, *root, *run);
    gdx3ds_nrr_trk_stats.ticks += svcGetSystemTick() - t0;
    return rc;
}

} // namespace

extern "C" void gdx3ds_nrr_cat_init_trk(void) {
    gdx3ds_nrr_set_draw(NRR_CAT_TRK, NrrTrkHotDraw);
    gdx3ds_nrr_set_peek(NRR_CAT_TRK, NrrTrkHotPeek);
}
