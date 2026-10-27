/* gdx3ds_track_resident.cpp — [native] track_resident: the load-time course bake and its GPU
 * copy (NEXT.md C5 Stage 3c; levers, receipts and the model: gdx3ds_track_resident.h). The draw
 * side lives in gdx3ds_track_retained.cpp, which owns the replay's slot state.
 *
 * Threads. The bake runs on the game thread inside Course_Init and publishes an immutable set
 * through one atomic pointer; the render thread exchanges it out at its next retained run and
 * becomes the set's only owner. ship6: every set lives in one of the two slots of a fixed linear
 * arena reserved once (no per-course heap or linear allocation; see "the arena" below), and its
 * vertices are the GPU's buffer directly. A set the render side never picked up (two loads in a
 * row) is taken back by the game thread at its next bake. */
#include "gdx3ds_track_resident.h"

#include <3ds.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gdx3ds_mesh.h"
#include "gdx3ds_native_cfg.h"
#include "nrr/gdx3ds_nrr_trk.h"
#include "nrr/gdx3ds_nrr_crumb.h" /* n60hwdiag */

extern "C" void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" int gdx3ds_config_loaded(void) __attribute__((weak));
extern "C" int gCourseIndex __attribute__((weak));
/* HAZARD CRUMBS (native/n3dsfix): the New3DS attract-demo freeze lines up with this file's
 * course-4 bake/adopt every time it has been captured; these two durable (synchronously
 * SD-flushed) crumbs bracket the only two calls here that leave CPU-only code (linearAlloc's
 * citro3d-linear-heap lock, and the dcache/GPU flush in gdx3ds_mesh_backend_flush_resident),
 * so a freeze that stops one line short of "post-adopt-flush" points straight at one of those
 * two instead of at the surrounding CPU-only baking math. [debug] hazard_crumbs (default 0). */
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern "C" void gdx3ds_filelog_flush(void) __attribute__((weak));
static int HazardCrumbsOn() {
    static int on = -1;
    if (on < 0) {
        on = (gdx3ds_config_get_int != nullptr &&
              gdx3ds_config_get_int("debug", "hazard_crumbs", 0) != 0) ? 1 : 0;
    }
    return on;
}
static void HazardCrumb(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void HazardCrumb(const char* fmt, ...) {
    if (!HazardCrumbsOn() || gdx3ds_filelog_write == nullptr) {
        return;
    }
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        gdx3ds_filelog_write(buf, (size_t)n);
        if (gdx3ds_filelog_flush != nullptr) {
            gdx3ds_filelog_flush();
        }
    }
}

namespace {

constexpr uint32_t kTrackFlagCorr = 0x80000000u; /* TRACK_FLAG_80000000 */
constexpr uint32_t kTrackJoinStartMask = 0x6000u;    /* TRACK_CHUNK_JOIN_TRANSITION_START_MASK */
constexpr uint32_t kTrackJoinPreviousEnd = 0x2000u;  /* TRACK_CHUNK_JOIN_PREVIOUS_END */
constexpr int kLayouts = 4;
constexpr int kMaxTables = 64;
constexpr uint32_t kMaxResidentVerts = 65536; /* u16 indices */
constexpr uint16_t kTexScaleS = 0x8000;       /* the walk's gSPTexture(0x8000, 0xFFFF, ..., G_ON) */
constexpr uint16_t kTexScaleT = 0xFFFF;

struct Config {
    bool latched;
    int level;
    bool verify;
    int maxErr;
    int window;
    uint32_t atlasRings; /* n60atlas: ring copies reserved after the bake's rings ([native] nrr_trk_atlas) */
};
Config sCfg = { false, 0, false, 125, 32, 0 };
/* n60atlas: rings of atlas-slot copies the render side may add behind the bake's rings (nrr_trk_atlas). */
constexpr uint32_t kAtlasRingCopies = 512;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    if (gdx_dbg_logf == nullptr) {
        return;
    }
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

void LatchConfig() {
    if (sCfg.latched || gdx3ds_config_get_int == nullptr) {
        return;
    }
    if (gdx3ds_config_loaded != nullptr && !gdx3ds_config_loaded()) {
        return;
    }
    sCfg.latched = true;
    sCfg.level = gdx3ds_native_raw("track_resident", 0) != 0 ? 1 : 0;
    sCfg.verify = gdx3ds_native_raw("track_resident_verify", 0) != 0;
    sCfg.maxErr = gdx3ds_native_raw("track_resident_maxerr", 125);
    sCfg.window = gdx3ds_native_raw("track_resident_window", 32);
    sCfg.window = sCfg.window < 1 ? 1 : (sCfg.window > 512 ? 512 : sCfg.window);
    sCfg.atlasRings = gdx3ds_native_raw("nrr_trk_atlas", 0) != 0 ? kAtlasRingCopies : 0u;
    if (sCfg.level == 0) {
        return;
    }
    const int track = gdx3ds_native_lever("track", 0);
    const int retained = gdx3ds_native_raw("track_retained", 0);
    if (track != 1 || retained != 1) {
        Log("[trkres] [native] track_resident=1 needs track=1 and track_retained=1 (track=%d track_retained=%d): off",
            track, retained);
        sCfg.level = 0;
        return;
    }
    Log("[trkres] init level=1 verify=%d maxerr=%d window=%d (course rings baked at load, drawn from a "
        "resident buffer with the exact eye offset `xoff`)",
        sCfg.verify ? 1 : 0, sCfg.maxErr, sCfg.window);
}

/* ---- ship6: the arena (one linear block, reserved once) ------------------------------------
 *
 * Card-6 hardware (Old 3DS, ~10 min, 6 course loads): `[alloc-fail] malloc 96768` in BakeCourse
 * with 1.6 MB of heap free -- fragmentation, not exhaustion -- and a `realloc 156672` of the same
 * ring growth one course later; the course silently lost the resident path and the previous
 * course's set stayed adopted. Every array a bake writes now lives in this arena instead.
 *
 * Capacities, worst case over every course (ship6 census of `[trkres] bake` / `[nrrtrk] index`
 * receipts, emulator + hardware: all 24 stock courses, the 12 Expansion Kit disk courses, X cup,
 * Death Race, ending): chunks max 495 (35 Big Blue 3; stock 422, 23 Big Hand) against
 * gSegmentChunks' 1025; rings max 1659 (39 Fire Field 2; stock 1657, 20 Space Plant) against
 * kArenaRings 2048; NRR index max 32928 keys (343 chunks x 4 U tables; 495 x 4 would be 47520)
 * against kArenaIndexKeys 65536. A user-made (EK editor) course past a capacity skips its bake
 * (the per-frame path draws it) and says so.
 *
 * Two slots, because the render side keeps drawing the adopted set (and the GPU keeps reading
 * its vertices for a few frames) while the game thread bakes the next one (the EK editor rebakes
 * a course it is drawing). A slot's state: FREE -> GAME (the game thread bakes / publishes it) ->
 * RENDER (adopted) -> RETIRED (the next adoption replaced it; the GPU may still read it) -> GAME
 * again once kSlotReuseFrames game frames have passed. The game thread is the only writer of FREE
 * / RETIRED -> GAME; the render side of GAME -> RENDER -> RETIRED. Linear memory: the heap is the
 * Old 3DS's tight pool (card-6 race: heapFree 3-20 KB), linear had ~6.5-9.9 MB free in the race. */
constexpr uint32_t kArenaChunks = 1025;      /* gSegmentChunks[1025] (decomp-3ds-snapshot.patch) */
constexpr uint32_t kArenaRings = 2048;       /* per slot; the largest course (39 Fire Field 2) bakes 1659 */
constexpr uint32_t kArenaHash = 4096;        /* power of two >= 2 * kArenaChunks (BuildHash) */
constexpr uint32_t kArenaIndexKeys = 65536;  /* NRR index keys (24 per chunk and U table); largest 32928 */
constexpr uint32_t kArenaSlots = 2;
constexpr uint32_t kSlotReuseFrames = 16;    /* game frames between a slot's retirement and its reuse */

enum SlotState : int { SLOT_FREE = 0, SLOT_GAME = 1, SLOT_RENDER = 2, SLOT_RETIRED = 3 };

struct ArenaSlot {
    GdxTrkResSet set;     /* the header; its arrays point into this slot's part of the block */
    uint32_t vertRings;   /* rings `set.verts` holds: kArenaRings + the atlas copies */
    int state;            /* SlotState (atomic) */
    uint32_t retireFrame; /* gGameFrameCount-based frame of the adoption that retired it */
};

struct ChunkSrc;

struct BakeScratch {
    ChunkSrc* src;   /* the bake's per-chunk source table (kArenaChunks) */
    uint16_t* indexRing;    /* the NRR index's key -> ring table (kArenaIndexKeys) */
    int8_t* chunkTable;     /* the NRR index's per-chunk U table (kArenaChunks) */
    uint16_t* joinRing;     /* the NRR index's per-chunk join ring (kArenaChunks) */
};

struct Arena {
    void* block;      /* one linearAlloc, never freed while the process runs */
    uint32_t bytes;
    bool tried;       /* reserve attempted (a failure is logged once and turns the lever off) */
    ArenaSlot slot[kArenaSlots];
    BakeScratch scratch;
    GdxNrrTrkIndex index; /* the one NRR index (game thread only, rebuilt per bake) */
};
Arena sArena = {};
/* published "no bake": the render side drops the set it holds (a skipped bake must not leave the
 * previous course's set drawing the new course) */
GdxTrkResSet sClearSet = { -1, 0, -1, 0, 0, nullptr, nullptr, nullptr, 0, nullptr, 0, { 0 }, 0.0f };

int SlotStateLoad(const ArenaSlot& a) {
    return __atomic_load_n(&a.state, __ATOMIC_ACQUIRE);
}

void SlotStateStore(ArenaSlot& a, int state) {
    __atomic_store_n(&a.state, state, __ATOMIC_RELEASE);
}

bool SlotStateClaim(ArenaSlot& a, int from) {
    int expected = from;
    return __atomic_compare_exchange_n(&a.state, &expected, (int)SLOT_GAME, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

uint32_t AlignUp16(uint32_t v) {
    return (v + 15u) & ~15u;
}

/* ---- the bake (game thread) ---------------------------------------------------------------- */

GdxTrkResSet* sPending = nullptr; /* published, not yet adopted (atomic exchange only) */
uint32_t sGeneration = 0;

/* A set the render side never adopted (or a bake that did not finish): its slot is free again. */
void ReleaseUnadopted(GdxTrkResSet* s) {
    if (s == nullptr || s->slot < 0 || s->slot >= (int32_t)kArenaSlots) {
        return;
    }
    SlotStateStore(sArena.slot[s->slot], SLOT_FREE);
}

/* A ring's key: the positions of its vertices 0 and 4 (referencePos1 and 2, distinct per chunk);
 * FindChunk then compares all eight, so a collision costs a compare, never a wrong match. */
uint32_t RingHash(const int16_t (*ob)[3]) {
    uint32_t h = 0x9E3779B9u;
    for (int k = 0; k < 8; k += 4) {
        for (int c = 0; c < 3; c++) {
            h = (h ^ (uint16_t)ob[k][c]) * 0x01000193u;
            h ^= h >> 15;
        }
    }
    return h;
}

/* The interpreter's final short U/V of a loaded slot (GfxSpVertex at the walk's scale), and of an
 * ST-patched one (SpModifyVertex writes VERTEX_MODIFIED_ST's shorts verbatim). */
void FinalUvScaled(const Gdx3dsVtxIn& v, int16_t uv[2]) {
    uv[0] = (int16_t)(((int)v.tc[0] * (int)kTexScaleS) >> 16);
    uv[1] = (int16_t)(((int)v.tc[1] * (int)kTexScaleT) >> 16);
}

void FinalUvPatched(const Gdx3dsVtxIn& v, int16_t uv[2]) {
    /* course.c VERTEX_MODIFIED_ST(s, t) = (((s) << 15) & 0xFFFF0000) | ((t) & 0xFFFF) */
    const uint32_t st = (((uint32_t)((int32_t)v.tc[0] << 15)) & 0xFFFF0000u) | ((uint32_t)(int32_t)v.tc[1] & 0xFFFFu);
    uv[0] = (int16_t)(st >> 16);
    uv[1] = (int16_t)st;
}

struct Builder {
    GdxTrkResSet* s;
    uint32_t cap; /* rings */
    bool failed;
    GdxNrrTrkIndex* nrr; /* n60 nrr_trk: (chunk, layout, table, corr, patched) -> ring; NULL = not built */
};

/* ship6: the slot's fixed capacity (no growth); false = the course is past the arena. */
bool Reserve(Builder& b, uint32_t rings) {
    return b.s->nRings + rings <= b.cap;
}

/* Append ring (raw game output + final U/V) to chunk i unless an identical one is there. Returns
 * the ring (new or the identical one), GDX_NRR_TRK_NO_RING when the bake ran out of room. */
uint32_t AddVariant(Builder& b, uint32_t chunk, const Gdx3dsVtxIn* raw, bool patched, uint8_t kind) {
    GdxTrkResSet& s = *b.s;
    GdxTrkResChunk& ch = s.chunks[chunk];
    Gdx3dsVtxIn ring[8];
    for (int k = 0; k < 8; k++) {
        ring[k] = raw[k];
        int16_t uv[2];
        if (patched) {
            FinalUvPatched(raw[k], uv);
        } else {
            FinalUvScaled(raw[k], uv);
        }
        ring[k].tc[0] = uv[0];
        ring[k].tc[1] = uv[1];
    }
    for (uint32_t r = ch.firstRing; r < ch.firstRing + ch.nRings; r++) {
        if (memcmp(&s.verts[r * 8u], ring, sizeof(ring)) == 0) {
            return r;
        }
    }
    if ((s.nRings + 1u) * 8u > kMaxResidentVerts || !Reserve(b, 1)) {
        b.failed = true;
        return GDX_NRR_TRK_NO_RING;
    }
    memcpy(&s.verts[s.nRings * 8u], ring, sizeof(ring));
    s.rings[s.nRings].chunk = (uint16_t)chunk;
    s.rings[s.nRings].kind = kind;
    s.rings[s.nRings].pad = 0;
    s.nRings++;
    ch.nRings++;
    s.kindCount[kind & 15u]++;
    return s.nRings - 1u;
}

struct ChunkSrc {
    uint32_t info;
    int layout;
    int table; /* index into the course's distinct tables, -1 = TRACK_TYPE_NONE */
};

int Wrap(int j, int n) {
    j %= n;
    return j < 0 ? j + n : j;
}

/* n60 nrr_trk: the lean walk names a ring by what the walk would have loaded; record the ring the
 * bake made (or found identical) for that key. */
void NrrNoteMode(Builder& b, int chunk, int layout, int table, uint32_t vmode, uint32_t ring) {
    GdxNrrTrkIndex* x = b.nrr;
    if (x == nullptr || ring == GDX_NRR_TRK_NO_RING || table < 0 || (uint32_t)table >= x->nTables) {
        return;
    }
    const uint32_t k =
        (((uint32_t)chunk * 4u + (uint32_t)layout) * x->nTables + (uint32_t)table) * GDX_NRR_TRK_VMODES + vmode;
    x->ring[k] = (uint16_t)ring;
}

void NrrNote(Builder& b, int chunk, int layout, int table, int corr, int patched, uint32_t ring) {
    NrrNoteMode(b, chunk, layout, table, (uint32_t)corr * 2u + (uint32_t)patched, ring);
}

/* A ring the walk loaded with one shape's vertex order and then ST-patched with the other position
 * class's formula (a road/cylinder/half-pipe ring under the pipe's patch, or back): the loaded
 * positions with the patching layout's final U/V. Only for flagged chunks (the only ones patched). */
constexpr int kLayoutPipe = 1;
int OtherClassLayout(int layout) {
    return layout == kLayoutPipe ? 0 : kLayoutPipe;
}

void BakeCrossPatch(Builder& b, int i, int layout, int table, const int16_t* d, int corr, uint8_t kind) {
    static const Gdx3dsVtxIn kTemplate = { { 0, 0, 0 }, 0, { 0, 0 }, { 0, 0, 0, 255 } };
    Gdx3dsVtxIn uv[8];
    Gdx3dsVtxIn pos[8];
    gdxtrk_game_ring(i, layout, d, corr, -1, &kTemplate, uv);
    gdxtrk_game_ring(i, OtherClassLayout(layout), d, corr, -1, &kTemplate, pos);
    for (int k = 0; k < 8; k++) {
        memcpy(uv[k].ob, pos[k].ob, sizeof(uv[k].ob));
    }
    const uint32_t ring = AddVariant(b, (uint32_t)i, uv, true, (uint8_t)(kind | GDXTRK_RES_PATCHED | GDXTRK_RES_FLAYOUT));
    NrrNoteMode(b, i, layout, table, 4u + (uint32_t)corr, ring);
}

void BakeChunk(Builder& b, int i, int n, const ChunkSrc* src, const int16_t (*tables)[5], int nTables) {
    GdxTrkResSet& s = *b.s;
    GdxTrkResChunk& ch = s.chunks[i];
    ch.firstRing = s.nRings;
    ch.nRings = 0;
    /* the layouts and tables a ring of chunk i can carry: those of the chunks within the window */
    uint32_t layoutMask = 0;
    uint64_t tableMask = 0;
    const int w = sCfg.window < n ? sCfg.window : n;
    for (int d = -w; d <= w; d++) {
        const ChunkSrc& o = src[Wrap(i + d, n)];
        layoutMask |= 1u << o.layout;
        if (o.table >= 0) {
            tableMask |= 1ull << o.table;
        }
    }
    const ChunkSrc& own = src[i];
    const bool flagged = (own.info & kTrackFlagCorr) != 0;
    static const Gdx3dsVtxIn kTemplate = { { 0, 0, 0 }, 0, { 0, 0 }, { 0, 0, 0, 255 } }; /* func_800A4BAC's cn */
    /* own layout and table first, so a ring's census kind is the most native one that makes it */
    int layouts[kLayouts];
    int nl = 0;
    layouts[nl++] = own.layout;
    for (int l = 0; l < kLayouts; l++) {
        if (l != own.layout && (layoutMask & (1u << l)) != 0) {
            layouts[nl++] = l;
        }
    }
    int order[kMaxTables];
    int nt = 0;
    if (own.table >= 0) {
        order[nt++] = own.table;
    }
    for (int t = 0; t < nTables; t++) {
        if (t != own.table && (tableMask & (1ull << t)) != 0) {
            order[nt++] = t;
        }
    }
    for (int li = 0; li < nl; li++) {
        for (int ti = 0; ti < nt; ti++) {
            for (int corr = 0; corr <= (flagged ? 1 : 0); corr++) {
                Gdx3dsVtxIn raw[8];
                gdxtrk_game_ring(i, layouts[li], tables[order[ti]], corr, -1, &kTemplate, raw);
                uint8_t kind = 0;
                kind |= corr ? GDXTRK_RES_CORR : 0;
                kind |= order[ti] != own.table ? GDXTRK_RES_FTABLE : 0;
                kind |= layouts[li] != own.layout ? GDXTRK_RES_FLAYOUT : 0;
                const uint32_t plain = AddVariant(b, (uint32_t)i, raw, false, kind);
                NrrNote(b, i, layouts[li], order[ti], corr, 0, plain);
                if (flagged) {
                    const uint32_t pat = AddVariant(b, (uint32_t)i, raw, true, (uint8_t)(kind | GDXTRK_RES_PATCHED));
                    NrrNote(b, i, layouts[li], order[ti], corr, 1, pat);
                    if (b.nrr != nullptr && (layoutMask & (1u << OtherClassLayout(layouts[li]))) != 0) {
                        BakeCrossPatch(b, i, layouts[li], order[ti], tables[order[ti]], corr, kind);
                    }
                }
            }
        }
    }
}

/* ---- n60trk2 nrr_trk_joins: the tunnel-mouth join vertices -------------------------------------
 *
 * Course_SetupChunkPipeTunnelJoinVtx writes five vertices (slots 16-20) whose positions are a function
 * of the chunk alone; the join templates draw them with a combiner that reads no texel. The source
 * chunk is the backward walk's working chunk when it starts a join other than PREVIOUS_END, or the
 * forward walk's `next` after a PREVIOUS_END chunk (merge LOD may skip up to the window). Appended after the chunk's rings (firstRing, the hash and the
 * bound stay the first ring's); never deduplicated against a ring. */
bool JoinCandidate(int i, int n, const ChunkSrc* src) {
    const uint32_t own = src[i].info & kTrackJoinStartMask;
    if (own != 0 && own != kTrackJoinPreviousEnd) {
        return true; /* the backward walk's join: the working chunk's own vertices */
    }
    const int w = sCfg.window < n ? sCfg.window : n;
    for (int d = 1; d <= w; d++) {
        if ((src[Wrap(i - d, n)].info & kTrackJoinStartMask) == kTrackJoinPreviousEnd) {
            return true; /* the forward walk's join: the (merged) next chunk's vertices */
        }
    }
    return false;
}

void BakeJoin(Builder& b, int i, int n, const ChunkSrc* src) {
    static const Gdx3dsVtxIn kTemplate = { { 0, 0, 0 }, 0, { 0, 0 }, { 0, 0, 0, 255 } }; /* Course_Init's cn */
    GdxNrrTrkIndex* x = b.nrr;
    if (x == nullptr || x->joinRing == nullptr || !JoinCandidate(i, n, src)) {
        return;
    }
    GdxTrkResSet& s = *b.s;
    if ((s.nRings + 1u) * 8u > kMaxResidentVerts || !Reserve(b, 1)) {
        b.failed = true;
        return;
    }
    gdxtrk_game_join_ring(i, &kTemplate, &s.verts[s.nRings * 8u]);
    s.rings[s.nRings].chunk = (uint16_t)i;
    s.rings[s.nRings].kind = GDXTRK_RES_JOIN;
    s.rings[s.nRings].pad = 0;
    x->joinRing[i] = (uint16_t)s.nRings;
    x->nJoinRings++;
    s.nRings++;
    s.chunks[i].nRings++;
}

void BoundChunk(GdxTrkResSet& s, uint32_t i) {
    GdxTrkResChunk& ch = s.chunks[i];
    if (ch.nRings == 0) {
        ch.r = -1.0f;
        return;
    }
    const Gdx3dsVtxIn* v = &s.verts[ch.firstRing * 8u];
    for (int c = 0; c < 3; c++) {
        ch.lo[c] = v[0].ob[c];
        ch.hi[c] = v[0].ob[c];
    }
    for (int k = 1; k < 8; k++) {
        for (int c = 0; c < 3; c++) {
            ch.lo[c] = v[k].ob[c] < ch.lo[c] ? v[k].ob[c] : ch.lo[c];
            ch.hi[c] = v[k].ob[c] > ch.hi[c] ? v[k].ob[c] : ch.hi[c];
        }
    }
    float r2 = 0.0f;
    for (int c = 0; c < 3; c++) {
        ch.c[c] = 0.5f * ((float)ch.lo[c] + (float)ch.hi[c]);
    }
    for (int k = 0; k < 8; k++) {
        float d2 = 0.0f;
        for (int c = 0; c < 3; c++) {
            const float d = (float)v[k].ob[c] - ch.c[c];
            d2 += d * d;
        }
        r2 = d2 > r2 ? d2 : r2;
    }
    ch.r = std::sqrt(r2) * 1.0001f + 0.01f; /* never smaller than the true radius */
}

bool BuildHash(GdxTrkResSet& s) {
    uint32_t size = 64;
    while (size < s.nChunks * 2u) {
        size *= 2;
    }
    if (size > kArenaHash || s.hash == nullptr) {
        return false; /* ship6: the slot's table (kArenaChunks bounds nChunks, so never past it) */
    }
    memset(s.hash, 0, size * sizeof(uint16_t));
    s.hashMask = size - 1u;
    for (uint32_t i = 0; i < s.nChunks; i++) {
        const GdxTrkResChunk& ch = s.chunks[i];
        if (ch.nRings == 0) {
            continue;
        }
        int16_t ob[8][3];
        for (int k = 0; k < 8; k++) {
            memcpy(ob[k], s.verts[ch.firstRing * 8u + k].ob, sizeof(ob[k]));
        }
        uint32_t h = RingHash(ob) & s.hashMask;
        while (s.hash[h] != 0) {
            h = (h + 1u) & s.hashMask;
        }
        s.hash[h] = (uint16_t)(i + 1u);
    }
    return true;
}

/* ---- n60 nrr_trk: the bake's index for the lean walk (game thread only) ------------------------
 *
 * The lean walk (gdx3ds_nrr_trk_game.c) knows, for every ring it would have loaded, the chunk, the
 * walking shape's ring layout, the U table in force, the V choice and whether an ST patch rewrote
 * it: exactly the bake's enumeration key. The index maps that key to the ring the bake kept (the
 * identical ring when two keys produce the same bytes). Owned by the game thread, replaced at the
 * next bake; the render side never reads it (an op carries the ring indices). */
GdxNrrTrkIndex* sNrrIx = nullptr;
uint32_t sNrrIxBytes = 0;

/* ship6: the index's tables are the arena's scratch; only the verify copy (nrr_trk = 2) is heap. */
void NrrIndexFree(GdxNrrTrkIndex* x) {
    if (x == nullptr) {
        return;
    }
    free(const_cast<void*>(x->verts));
    x->verts = nullptr;
}

GdxNrrTrkIndex* NrrIndexBegin(int n, const ChunkSrc* src, const int16_t (*tables)[5], int nTables) {
    if (gdxtrk_nrr_level() == 0 || nTables <= 0) {
        return nullptr;
    }
    const size_t keys = (size_t)n * 4u * (size_t)nTables * (size_t)GDX_NRR_TRK_VMODES;
    if (keys > kArenaIndexKeys) {
        Log("[trkres] arena: NRR index skipped (chunks=%d tables=%d keys=%u > %u) -- the track pass walks "
            "the legacy groups",
            n, nTables, (unsigned)keys, kArenaIndexKeys);
        return nullptr;
    }
    GdxNrrTrkIndex* x = &sArena.index;
    memset(x, 0, sizeof(*x));
    x->nChunks = (uint32_t)n;
    x->nTables = (uint32_t)nTables;
    memcpy(x->tables, tables, (size_t)nTables * sizeof(tables[0]));
    x->ring = sArena.scratch.indexRing;
    x->chunkTable = sArena.scratch.chunkTable;
    memset(x->ring, 0xFF, keys * sizeof(uint16_t));
    if (gdxtrk_nrr_joins()) {
        x->joinRing = sArena.scratch.joinRing;
        memset(x->joinRing, 0xFF, (size_t)n * sizeof(uint16_t));
    }
    for (int i = 0; i < n; i++) {
        x->chunkTable[i] = (int8_t)src[i].table;
    }
    sNrrIxBytes = (uint32_t)(keys * sizeof(uint16_t) + (size_t)n + (x->joinRing != nullptr ? (size_t)n * 2u : 0u));
    return x;
}

void NrrIndexPublish(GdxNrrTrkIndex* x, const GdxTrkResSet& s) {
    if (x == nullptr) {
        return;
    }
    x->gen = sGeneration + 1u; /* the generation gdx3ds_track_resident_bake is about to give the set */
    x->nRings = s.nRings;
    if (gdxtrk_nrr_level() == 2) {
        const size_t bytes = (size_t)s.nRings * 8u * sizeof(Gdx3dsVtxIn);
        void* v = malloc(bytes);
        if (v != nullptr) {
            memcpy(v, s.verts, bytes);
            x->verts = v;
        }
    }
    sNrrIx = x; /* the one index (sArena.index): the bake released the previous course's first */
}

/* n60atlas [debug] atlas_census: the course's chunks by (shape, type) -- the track materials the walk
 * can load -- and the boundaries between consecutive typed chunks of different (shape, type). */
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));

__attribute__((noinline, cold)) void CensusChunks(const ChunkSrc* src, int n) {
    if (gdx3ds_config_get_int == nullptr || gdx3ds_config_get_int("debug", "atlas_census", 0) == 0) {
        return;
    }
    uint16_t count[8][64];
    memset(count, 0, sizeof(count));
    int none = 0;
    int bounds = 0;
    int prev = -1;
    for (int i = 0; i < n; i++) {
        const uint32_t shape = (src[i].info & 0x1C0u) >> 6;
        const uint32_t type = src[i].info & 0x3Fu;
        if (type == 0x3Fu) {
            none++;
            continue;
        }
        count[shape][type]++;
        const int cur = (int)(src[i].info & 0x1FFu);
        bounds += (prev >= 0 && prev != cur) ? 1 : 0;
        prev = cur;
    }
    char b[512];
    int len = snprintf(b, sizeof(b), "[atlascen] course=%d chunks=%d none=%d bounds=%d types:", &gCourseIndex != nullptr ? gCourseIndex : -1,
                       n, none, bounds);
    for (int sh = 0; sh < 8; sh++) {
        for (int t = 0; t < 64; t++) {
            if (count[sh][t] != 0 && len < (int)sizeof(b) - 16) {
                len += snprintf(b + len, sizeof(b) - (size_t)len, " %d.%d=%u", sh, t, (unsigned)count[sh][t]);
            }
        }
    }
    Log("%s", b);
}

/* ship6: why a bake gave no set (the caller logs and clears accordingly). */
enum BakeResult { BAKE_OK = 0, BAKE_NO_COURSE = 1, BAKE_OVERFLOW = 2 };

BakeResult BakeCourse(ArenaSlot& slot, int* chunksOut) {
    const int n = gdxtrk_game_chunk_count();
    *chunksOut = n;
    if (n <= 0) {
        return BAKE_NO_COURSE;
    }
    if ((uint32_t)n > kArenaChunks) {
        return BAKE_OVERFLOW;
    }
    ChunkSrc* src = sArena.scratch.src;
    GdxTrkResSet* s = &slot.set;
    GdxTrkResChunk* chunks = s->chunks;
    GdxTrkResRing* rings = s->rings;
    Gdx3dsVtxIn* verts = s->verts;
    uint16_t* hash = s->hash;
    const int32_t slotIndex = s->slot;
    memset(s, 0, sizeof(*s));
    s->slot = slotIndex;
    s->chunks = chunks;
    s->rings = rings;
    s->verts = verts;
    s->hash = hash;
    int16_t tables[kMaxTables][5];
    int nTables = 0;
    memset(src, 0, (size_t)n * sizeof(ChunkSrc));
    for (int i = 0; i < n; i++) {
        src[i].info = gdxtrk_game_chunk_info(i);
        src[i].layout = gdxtrk_game_layout_of(src[i].info);
        src[i].table = -1;
        int16_t d[5];
        if (!gdxtrk_game_chunk_table(i, d)) {
            continue;
        }
        for (int t = 0; t < nTables; t++) {
            if (memcmp(tables[t], d, sizeof(d)) == 0) {
                src[i].table = t;
                break;
            }
        }
        if (src[i].table < 0 && nTables < kMaxTables) {
            memcpy(tables[nTables], d, sizeof(d));
            src[i].table = nTables++;
        }
    }
    CensusChunks(src, n);
    s->nChunks = (uint32_t)n;
    memset(s->chunks, 0, (size_t)n * sizeof(GdxTrkResChunk));
    Builder b = { s, kArenaRings, false, NrrIndexBegin(n, src, tables, nTables) };
    for (int i = 0; i < n && !b.failed; i++) {
        s->chunks[i].info9 = (uint16_t)(src[i].info & 0x1FFu); /* n60atlas: the chunk's shape | type */
        BakeChunk(b, i, n, src, tables, nTables);
        BakeJoin(b, i, n, src);
        s->maxVariants = s->chunks[i].nRings > s->maxVariants ? s->chunks[i].nRings : s->maxVariants;
    }
    if (b.failed || !BuildHash(*s)) {
        NrrIndexFree(b.nrr);
        return BAKE_OVERFLOW;
    }
    for (uint32_t i = 0; i < s->nChunks; i++) {
        BoundChunk(*s, i);
    }
    NrrIndexPublish(b.nrr, *s);
    return BAKE_OK;
}

uint32_t CountKinds(const GdxTrkResSet& s, uint8_t bit) {
    uint32_t t = 0;
    for (int k = 0; k < 16; k++) {
        t += (k & bit) != 0 ? s.kindCount[k] : 0;
    }
    return t;
}

/* ---- ship6: arena reserve + slot hand-off (game thread) ------------------------------------- */

uint32_t SlotBytes(uint32_t vertRings) {
    return AlignUp16(vertRings * 8u * (uint32_t)sizeof(Gdx3dsVtxIn)) +
           AlignUp16(kArenaChunks * (uint32_t)sizeof(GdxTrkResChunk)) +
           AlignUp16(kArenaRings * (uint32_t)sizeof(GdxTrkResRing)) + AlignUp16(kArenaHash * (uint32_t)sizeof(uint16_t));
}

uint32_t ScratchBytes() {
    return AlignUp16(kArenaChunks * (uint32_t)sizeof(ChunkSrc)) + AlignUp16(kArenaIndexKeys * (uint32_t)sizeof(uint16_t)) +
           AlignUp16(kArenaChunks) + AlignUp16(kArenaChunks * (uint32_t)sizeof(uint16_t));
}

/* Carve the next `bytes` (16-aligned) off the block. */
void* Carve(uint8_t*& at, uint32_t bytes) {
    void* p = at;
    at += AlignUp16(bytes);
    return p;
}

void CarveSlot(uint8_t*& at, int index, uint32_t vertRings) {
    ArenaSlot& a = sArena.slot[index];
    memset(&a, 0, sizeof(a));
    a.vertRings = vertRings;
    a.set.slot = index;
    a.set.verts = (Gdx3dsVtxIn*)Carve(at, vertRings * 8u * (uint32_t)sizeof(Gdx3dsVtxIn));
    a.set.chunks = (GdxTrkResChunk*)Carve(at, kArenaChunks * (uint32_t)sizeof(GdxTrkResChunk));
    a.set.rings = (GdxTrkResRing*)Carve(at, kArenaRings * (uint32_t)sizeof(GdxTrkResRing));
    a.set.hash = (uint16_t*)Carve(at, kArenaHash * (uint32_t)sizeof(uint16_t));
    SlotStateStore(a, SLOT_FREE);
}

void CarveScratch(uint8_t*& at) {
    sArena.scratch.src = (ChunkSrc*)Carve(at, kArenaChunks * (uint32_t)sizeof(ChunkSrc));
    sArena.scratch.indexRing = (uint16_t*)Carve(at, kArenaIndexKeys * (uint32_t)sizeof(uint16_t));
    sArena.scratch.chunkTable = (int8_t*)Carve(at, kArenaChunks);
    sArena.scratch.joinRing = (uint16_t*)Carve(at, kArenaChunks * (uint32_t)sizeof(uint16_t));
}

/* Once per process: one linearAlloc for both slots and the scratch. false = no arena (logged once;
 * the lever is off from here, the per-frame path draws every course). */
bool ArenaReserve() {
    if (sArena.block != nullptr) {
        return true;
    }
    if (sArena.tried) {
        return false;
    }
    sArena.tried = true;
    uint32_t vertRings = kArenaRings + sCfg.atlasRings;
    vertRings = vertRings * 8u > kMaxResidentVerts ? kMaxResidentVerts / 8u : vertRings;
    const uint32_t slotBytes = SlotBytes(vertRings);
    const uint32_t bytes = slotBytes * kArenaSlots + ScratchBytes();
    const unsigned linBefore = (unsigned)(linearSpaceFree() / 1024u);
    void* p = linearAlloc(bytes);
    if (p == nullptr) {
        Log("[trkres] arena: linearAlloc(%u) FAILED (linfree=%uK) -- track_resident off, per-frame path", bytes,
            linBefore);
        sCfg.level = 0;
        return false;
    }
    sArena.block = p;
    sArena.bytes = bytes;
    uint8_t* at = (uint8_t*)p;
    for (uint32_t i = 0; i < kArenaSlots; i++) {
        CarveSlot(at, (int)i, vertRings);
    }
    CarveScratch(at);
    Log("[trkres] arena reserved %u bytes linear (%u slots x %u: chunks<=%u rings<=%u (+%u atlas) hash<=%u; "
        "scratch %u: index keys<=%u) linfree=%uK->%uK",
        bytes, kArenaSlots, slotBytes, kArenaChunks, kArenaRings, vertRings - kArenaRings, kArenaHash, ScratchBytes(),
        kArenaIndexKeys, linBefore, (unsigned)(linearSpaceFree() / 1024u));
    return true;
}

/* The game frame now (the counter the render side's `frame` comes from: GDXTRK_BEGIN). */
extern "C" unsigned int gGameFrameCount __attribute__((weak));

bool SlotReusable(const ArenaSlot& a) {
    if (&gGameFrameCount == nullptr) {
        return true;
    }
    return (int32_t)((uint32_t)gGameFrameCount - a.retireFrame) >= (int32_t)kSlotReuseFrames;
}

/* A FREE slot, else a RETIRED one the GPU is done with. -1 = both busy (the adopted set and one
 * retired a moment ago: two bakes within kSlotReuseFrames of an adoption). */
int ClaimSlot() {
    for (uint32_t i = 0; i < kArenaSlots; i++) {
        if (SlotStateClaim(sArena.slot[i], SLOT_FREE)) {
            return (int)i;
        }
    }
    for (uint32_t i = 0; i < kArenaSlots; i++) {
        ArenaSlot& a = sArena.slot[i];
        if (SlotStateLoad(a) == SLOT_RETIRED && SlotReusable(a) && SlotStateClaim(a, SLOT_RETIRED)) {
            return (int)i;
        }
    }
    return -1;
}

/* Take back a set published but never adopted: its slot is the game thread's again. */
void ReclaimPending() {
    GdxTrkResSet* old = __atomic_exchange_n(&sPending, (GdxTrkResSet*)nullptr, __ATOMIC_ACQ_REL);
    ReleaseUnadopted(old);
}

void Publish(GdxTrkResSet* s) {
    GdxTrkResSet* old = __atomic_exchange_n(&sPending, s, __ATOMIC_ACQ_REL);
    ReleaseUnadopted(old); /* nullptr after ReclaimPending; kept for safety */
}

/* A bake that could not run: the render side drops the previous course's set at its next run. */
void PublishClear() {
    Publish(&sClearSet);
}

/* ---- the render side ----------------------------------------------------------------------- */

GdxTrkResView sView = { nullptr, nullptr, 0, 0 };
GdxTrkResSet* sOwned = nullptr;

/* The adopted set's slot goes to RETIRED: the GPU may read it until kSlotReuseFrames pass. */
void RetireOwned(uint32_t frame) {
    if (sOwned == nullptr) {
        return;
    }
    ArenaSlot& a = sArena.slot[sOwned->slot];
    a.retireFrame = frame;
    SlotStateStore(a, SLOT_RETIRED);
    sOwned = nullptr;
}

void Adopt(GdxTrkResSet* s, uint32_t frame) {
    HazardCrumb("[hzc] trkres adopt enter gen=%u course=%d frame=%lu\n", s->generation, (int)s->course,
                (unsigned long)frame);
    RetireOwned(frame);
    sView.set = nullptr;
    sView.linear = nullptr;
    sView.nVerts = 0;
    sView.ringCap = 0;
    if (s->slot < 0 || s->nRings == 0) {
        return; /* the clear set, or an empty bake: the per-frame path draws */
    }
    ArenaSlot& a = sArena.slot[s->slot];
    SlotStateStore(a, SLOT_RENDER);
    sOwned = s;
    const uint32_t bytes = s->nRings * 8u * (uint32_t)sizeof(Gdx3dsVtxIn);
    /* n60atlas: room for the atlas slots' ring copies behind the bake's rings (u16 indices) */
    uint32_t ringCap = s->nRings + sCfg.atlasRings;
    ringCap = ringCap > a.vertRings ? a.vertRings : ringCap;
    void* p = s->verts; /* ship6: the slot's vertices are the GPU's buffer (linear, no copy) */
    if (GDX_NRR_CRUMBS_ON()) {
        char b[96];
        gdx3ds_nrr_crumb_buf(b, sizeof(b), "ring", p);
        gdx3ds_nrr_crumb("trk adopt gen=%u slot=%d (%lu bytes) %s linfree=%uK (render side)", s->generation,
                         (int)s->slot, (unsigned long)bytes, b, gdx3ds_nrr_crumb_linfree_kb());
    }
    HazardCrumb("[hzc] trkres adopt pre-flush_resident gen=%u\n", s->generation);
    gdx3ds_mesh_backend_flush_resident(p, bytes);
    HazardCrumb("[hzc] trkres adopt post-flush_resident gen=%u\n", s->generation);
    sView.set = s;
    sView.linear = static_cast<const Gdx3dsVtxIn*>(p);
    sView.nVerts = s->nRings * 8u;
    sView.ringCap = ringCap;
    Log("[trkres] adopt gen=%u course=%d rings=%u verts=%u resident=%u bytes (linear arena slot %d)", s->generation,
        (int)s->course, s->nRings, sView.nVerts, bytes, (int)s->slot);
    HazardCrumb("[hzc] trkres adopt exit gen=%u\n", s->generation);
}

void LogBake(const GdxTrkResSet& s) {
    const uint32_t bytes = s.nRings * 8u * (uint32_t)sizeof(Gdx3dsVtxIn);
    const uint32_t side = s.nChunks * (uint32_t)sizeof(GdxTrkResChunk) + s.nRings * (uint32_t)sizeof(GdxTrkResRing) +
                          (s.hashMask + 1u) * 2u;
    Log("[trkres] bake gen=%u course=%d chunks=%u rings=%u variants/chunk=%.2f max=%u kinds own=%u corr=%u patched=%u "
        "ftable=%u flayout=%u | resident=%u bytes (+%u chunks/rings/hash) slot=%d of arena ms=%.2f window=%d",
        s.generation, (int)s.course, s.nChunks, s.nRings, s.nChunks ? (double)s.nRings / s.nChunks : 0.0, s.maxVariants,
        s.kindCount[0], CountKinds(s, GDXTRK_RES_CORR), CountKinds(s, GDXTRK_RES_PATCHED), CountKinds(s, GDXTRK_RES_FTABLE),
        CountKinds(s, GDXTRK_RES_FLAYOUT), bytes, side, (int)s.slot, (double)s.bakeMs, sCfg.window);
    if (sNrrIx != nullptr) {
        uint32_t filled = 0;
        const uint32_t keys = sNrrIx->nChunks * 4u * sNrrIx->nTables * GDX_NRR_TRK_VMODES;
        for (uint32_t k = 0; k < keys; k++) {
            filled += sNrrIx->ring[k] != GDX_NRR_TRK_NO_RING ? 1u : 0u;
        }
        Log("[nrrtrk] index gen=%u chunks=%u tables=%u keys=%u filled=%u bytes=%u verify_copy=%d join_rings=%u "
            "(%u resident bytes)",
            sNrrIx->gen, sNrrIx->nChunks, sNrrIx->nTables, keys, filled, sNrrIxBytes, sNrrIx->verts != nullptr ? 1 : 0,
            sNrrIx->nJoinRings, sNrrIx->nJoinRings * 8u * (uint32_t)sizeof(Gdx3dsVtxIn));
    }
}

} // namespace

extern "C" const GdxNrrTrkIndex* gdxtrk_nrr_index(void) {
    return sNrrIx;
}

extern "C" void gdx3ds_track_resident_reserve(void) {
    LatchConfig();
    if (sCfg.level != 0) {
        ArenaReserve();
    }
}

extern "C" void gdx3ds_track_resident_bake(void) {
    LatchConfig();
    if (sCfg.level == 0 || !ArenaReserve()) {
        return;
    }
    const uint64_t t0 = svcGetSystemTick();
    const int course = &gCourseIndex != nullptr ? gCourseIndex : -1;
    if (GDX_NRR_CRUMBS_ON()) {
        gdx3ds_nrr_crumb("trk bake > course=%d (game thread, Course_Init) linfree=%uK", course, gdx3ds_nrr_crumb_linfree_kb());
    }
    NrrIndexFree(sNrrIx); /* a new course: the old index names the old course's rings */
    sNrrIx = nullptr;
    ReclaimPending();
    const int slotIndex = ClaimSlot();
    if (slotIndex < 0) {
        Log("[trkres] arena busy: course=%d bake skipped (both slots in use within %u frames of an adoption) -- "
            "previous course's set dropped, per-frame path",
            course, kSlotReuseFrames);
        PublishClear();
        return;
    }
    ArenaSlot& slot = sArena.slot[slotIndex];
    HazardCrumb("[hzc] trkres bake pre-BakeCourse course=%d slot=%d\n", course, slotIndex);
    int chunks = 0;
    const BakeResult r = BakeCourse(slot, &chunks);
    HazardCrumb("[hzc] trkres bake post-BakeCourse result=%d\n", (int)r);
    GdxTrkResSet* s = &slot.set;
    if (GDX_NRR_CRUMBS_ON()) {
        gdx3ds_nrr_crumb("trk bake < result=%d slot=%d rings=%u chunks=%u index=%p index bytes=%u", (int)r, slotIndex,
                         s->nRings, s->nChunks, (void*)sNrrIx, sNrrIxBytes);
    }
    if (r != BAKE_OK) {
        SlotStateStore(slot, SLOT_FREE);
        if (r == BAKE_OVERFLOW) {
            Log("[trkres] arena overflow: course=%d chunks=%d rings>=%u (capacity chunks=%u rings=%u, %u resident "
                "vertices) bake skipped -- previous course's set dropped, per-frame path",
                course, chunks, s->nRings, kArenaChunks, kArenaRings, kMaxResidentVerts);
            PublishClear();
        }
        return;
    }
    s->generation = ++sGeneration;
    s->course = course;
    s->bakeMs = (float)((double)(svcGetSystemTick() - t0) / (double)SYSCLOCK_ARM11 * 1000.0);
    LogBake(*s);
    Publish(s);
}

namespace GdxTrkRes {

int Level() {
    LatchConfig();
    return sCfg.level;
}

bool Verify() {
    return sCfg.verify;
}

int MaxErr() {
    return sCfg.maxErr;
}

const GdxTrkResView* Acquire(uint32_t frame) {
    LatchConfig();
    if (sCfg.level == 0) {
        return nullptr;
    }
    GdxTrkResSet* s = __atomic_exchange_n(&sPending, (GdxTrkResSet*)nullptr, __ATOMIC_ACQ_REL);
    if (s != nullptr) {
        Adopt(s, frame);
    }
    return sView.set != nullptr ? &sView : nullptr;
}

int32_t FindChunk(const GdxTrkResSet& s, const Gdx3dsVtxIn* const v[8]) {
    int16_t ob[8][3];
    for (int k = 0; k < 8; k++) {
        memcpy(ob[k], v[k]->ob, sizeof(ob[k]));
    }
    uint32_t h = RingHash(ob) & s.hashMask;
    for (uint32_t probe = 0; probe <= s.hashMask; probe++) {
        const uint16_t e = s.hash[h];
        if (e == 0) {
            return -1;
        }
        const GdxTrkResChunk& ch = s.chunks[e - 1u];
        const Gdx3dsVtxIn* r = &s.verts[ch.firstRing * 8u];
        bool same = true;
        for (int k = 0; k < 8 && same; k++) {
            same = memcmp(r[k].ob, ob[k], sizeof(ob[k])) == 0;
        }
        if (same) {
            return (int32_t)(e - 1u);
        }
        h = (h + 1u) & s.hashMask;
    }
    return -1;
}

/* ship6: drops the view; the arena itself stays reserved for the process (one block, no churn). */
void Shutdown() {
    sView = { nullptr, nullptr, 0, 0 };
    if (sOwned != nullptr) {
        SlotStateStore(sArena.slot[sOwned->slot], SLOT_FREE);
        sOwned = nullptr;
    }
}

} // namespace GdxTrkRes
