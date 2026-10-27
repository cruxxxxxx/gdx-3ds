/* port/3ds/gfx/n64/gdx_hud_retain.cpp -- [native] hud_retain: run-level retention of the race
 * HUD, and [native] hudelem, the per-run HUD census (c5hud3, docs/research/native/
 * c5hud3-progress.md).
 *
 * WHERE IT RUNS. Inside c5s2's HUD replay (gdx_hud_replay.cpp HudReplayRun, run kind HUD) with
 * c5s2b's direct path on ([native] hud_direct=1). The game's HUD functions still run in full on
 * the game thread and still write their display list; c5s2's capture still moves the
 * recordable commands behind 0x5E markers. Retention changes how a run executes, and nothing
 * outside the run.
 *
 * THE IDEA. Measured (hudelem, canonical GP race): ~12 of the 14 HUD runs are byte-identical
 * to the same run of the previous task. A run is a pure function of
 *   (a) its words,
 *   (b) the interpreter registers it reads before writing them (computed per run below),
 *   (c) the TMEM span model -- only through the spans the run's own loads store, because a
 *       retainable run's every import samples a span one of its own loads stored,
 *   (d) its textures' source bytes (the DMA/native generation stamps), the texture-cache slots
 *       its imports hit, the classification epoch, the atlas placement epoch, the frame
 *       configuration and the VBO pool headroom.
 * When all of those match the recorded run, the run's effects are the recorded effects:
 *   - the backend calls (binds, samplers, shader, blend, depth, prim depth, alpha threshold,
 *     fog note, atlas arm, batch begins and draws), in order, with the recorded vertex floats
 *     copied into this frame's streamed pool at the recorded offsets (the GPU lifetime is the
 *     pool's own: it is reset per frame after the frame's GPU work, exactly as for the stock
 *     emission);
 *   - the TMEM span stores and debts of its loads (re-applied on the live span model, which is
 *     what the stock load does after its memoised resolution);
 *   - the LRU refresh of the cache slots its imports hit;
 *   - the registers it writes (applied from the recorded exit state).
 * A run that differs executes as before and is recorded again.
 *
 * (b) IS COMPUTED, NOT ASSUMED. HudRetainAnalyse walks the recorded words with a taint set per
 * register group: a group starts tainted by its own entry value; a full write clears the
 * taint, a partial or value-gated write keeps it (and adds its inputs); an op whose effect
 * leaves the interpreter (a draw, a load, a flush) adds the taints of everything it reads to
 * the compare set. At the end every group the run wrote adds its taint too (its recorded exit
 * value is applied). The compare set is therefore exactly the entry values the run's effects
 * depend on; the rest of the register file is neither compared nor touched. Level 2 checks the
 * argument on every would-be hit (below).
 *
 * VERIFY ([native] hud_retain=2). Every run whose fingerprint matches executes in full anyway,
 * recording a fresh log, and is compared with what the hit would have done: every backend call
 * and argument, every vertex float (bitwise), every aux record, every register group (written
 * groups against the recorded exit, the others against the entry), the span model and TMEM
 * (materialised with the debts) against the recorded loads applied to the entry state, and the
 * texture-cache LRU tail. `[hudr] vfy ... bad=0` is the in-process proof.
 *
 * [native] hudelem=1 (diagnostic): once every 512 tasks one [hudelem] line per HUD run (words
 * by class, rectangles' screen box, loads with format and tile extent, drains, imports, clock),
 * and on the receipt cadence how many runs were byte-identical to the previous task's. */
#include "gdx_interp.h"
#include "gdx3ds_replay.h" // [native] replay (C6): retained draws carry their tag

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_hud_record.h"
#include "gdx_log.h"
#include "gdx3ds_hotcold.h"

extern "C" long long gdx_ninterp_now(void);

#include "gdx3ds_hwprof.h" // hwprof [hwsite]: GDX3DS_HWS_* site ids

/* hwprof [hwsite] (port/3ds/include/gdx3ds_hwprof.h): per-call timers behind [debug] hwsite. The
 * seam returns 0 when the lever is off (one call per site); the host test stubs it. */
extern "C" long long gdx_ninterp_hws_begin(void);
extern "C" void gdx_ninterp_hws_end(int site, long long t0);
namespace {
struct InterpHwsScope {
    int site;
    long long t0;
    explicit InterpHwsScope(int s) : site(s), t0(gdx_ninterp_hws_begin()) {}
    ~InterpHwsScope() {
        if (t0 != 0) {
            gdx_ninterp_hws_end(site, t0);
        }
    }
    InterpHwsScope(const InterpHwsScope&) = delete;
    InterpHwsScope& operator=(const InterpHwsScope&) = delete;
};
} // namespace

namespace gdxn {

namespace {

constexpr uint32_t kElemEvery = 512; /* census cadence, in tasks */
constexpr uint32_t kRunSlots = 64;   /* runs per task that are identified/retained */
constexpr uint8_t kMissStreak = 4;   /* consecutive recordings with no hit before a slot cools down */
constexpr uint32_t kCoolTasks = 32;  /* tasks a cooled slot executes without recording */
/* Runs longer than this are not retained: the race HUD's longest run is ~100 words, and the
 * results screens' ~840-word table changes almost every frame, so recording it only costs. */
constexpr uint32_t kMaxRetainWords = 256;
constexpr size_t kPackTriFloats = 3u * GDX3DS_VBOPACK_STRIDE;

/* ---- register groups -------------------------------------------------------------------- */

enum RetGroup {
    RG_OM = 0,  /* other_mode_l/h */
    RG_CC,      /* combine_mode */
    RG_PRIM,    /* prim_color, prim_lod_fraction */
    RG_ENV,
    RG_FOG,     /* fog_color */
    RG_BLEND,
    RG_FILL,
    RG_PDEPTH,
    RG_KEY,     /* key_center, key_scale */
    RG_CONV,    /* convert_k */
    RG_SCIS,    /* scissor */
    RG_VP,      /* viewport */
    RG_VSC,     /* viewport_or_scissor_changed */
    RG_GRAY,    /* grayscale, grayscale_color */
    RG_FTI,     /* first_tile_index */
    RG_TXC,     /* textures_changed[2] */
    RG_TTL,     /* texture_to_load */
    RG_GEOM,    /* rsp geometry_mode */
    RG_XGEOM,   /* rsp extra_geometry_mode */
    RG_TSCALE,  /* rsp texture_scaling_factor */
    RG_FOGMW,   /* rsp fog_mul / fog_offset */
    RG_ZCI,     /* color_image_address == z_buf_address (FILLRECT's depth-clear test) */
    RG_RS0,     /* mRs: depth, decal, alpha, viewport, scissor, shader, texture unit 0 */
    RG_RS1,     /* mRs: texture unit 1 */
    RG_PACK,    /* the open packed batch */
    RG_LVXY,    /* loaded_vertices[64..67] x, y, z, w */
    RG_LVUV,    /* ... u, v */
    RG_LVCOL,   /* ... colour */
    RG_LVCLIP,  /* ... clip_rej, clip_out */
    RG_TRIMEMO, /* mTriMemo, mTriMemoValid */
    RG_TILEF0,  /* texture_tile[t]: the SETTILE fields, t = 0..7 */
    RG_TILES0 = RG_TILEF0 + 8, /* texture_tile[t]: uls, ult, lrs, lrt */
    RG_COUNT = RG_TILES0 + 8
};
static_assert(RG_COUNT <= 64, "register groups are a 64-bit mask");

/* [native] replay (C6): the tag a retained draw re-issues with. A batch the run BEGAN holds only
 * the run's rectangles -- a retainable run is rectangles only (a triangle is RU_OP) and a HUD
 * rectangle is screen space, exactly what the direct path tags it (HudDirectTri) -- so it is
 * GDX_REPLAY_TAG_SCREEN: pose-invariant, a replay keeps it as drawn (the HUD updates at the
 * render rate). A draw that flushes the batch open at the run's ENTRY (the scene's last batch,
 * drained by the run's first rectangle) draws this frame's triangles and takes the live tag. */
inline uint16_t RetainedReplayTag(bool begunHere, uint16_t live) {
    return begunHere ? static_cast<uint16_t>(GDX_REPLAY_TAG_SCREEN) : live;
}

inline uint64_t Bit(int g) {
    return 1ull << g;
}

const char* const kGroupName[RG_COUNT] = {
    "om", "cc", "prim", "env", "fog", "blend", "fill", "pdepth", "key", "conv", "scis", "vp", "vsc", "gray",
    "fti", "txc", "ttl", "geom", "xgeom", "tscale", "fogmw", "zci", "rs0", "rs1", "pack", "lvxy", "lvuv",
    "lvcol", "lvclip", "trimemo", "tf0", "tf1", "tf2", "tf3", "tf4", "tf5", "tf6", "tf7", "ts0", "ts1",
    "ts2", "ts3", "ts4", "ts5", "ts6", "ts7",
};

/* ---- recorded backend calls ------------------------------------------------------------- */

enum RetOp : uint8_t {
    EV_UNLOAD = 0,
    EV_LOAD,
    EV_SELTEX,
    EV_SAMPLER,
    EV_DEPTH,
    EV_DECAL,
    EV_VIEWPORT,
    EV_SCISSOR,
    EV_ALPHA,
    EV_PRIMDEPTH,
    EV_ALPHATHR,
    EV_FOGNOTE,
    EV_ATLASARM,
    EV_REFRESH,  /* i[0] = params index */
    EV_BEGIN,    /* i[0] = params index, i[1] = cap */
    EV_FLOATS,   /* i[0] = first triangle in the batch, i[1] = triangles, i[2] = float offset, i[3] = batch */
    EV_DRAW,     /* i[0] = triangles, i[1] = aux index, i[2] = batch */
    EV_CLEARDEPTH,
    EV_COUNT
};

struct RetEv {
    uint8_t op;
    uint8_t a, b, c;
    int32_t i[4];
    float f;
    const void* p;
};

/* A growable POD array (malloc/realloc: an allocation failure makes the run unretainable,
 * never an exception). */
template <typename T>
struct PodVec {
    T* d = nullptr;
    uint32_t n = 0;
    uint32_t cap = 0;
    bool Reserve(uint32_t want) {
        if (GDX_LIKELY(want <= cap)) {
            return true;
        }
        return ReserveSlow(want);
    }
    /* hotcold: growth happens while the records warm up, then never again */
    GDX_COLD bool ReserveSlow(uint32_t want) {
        uint32_t c = cap != 0 ? cap : 16u;
        while (c < want) {
            c *= 2u;
        }
        T* nd = static_cast<T*>(realloc(static_cast<void*>(d), sizeof(T) * c));
        if (nd == nullptr) {
            return false;
        }
        d = nd;
        cap = c;
        return true;
    }
    bool Push(const T& v) {
        if (!Reserve(n + 1u)) {
            return false;
        }
        d[n++] = v;
        return true;
    }
    T* Grow(uint32_t k) {
        if (!Reserve(n + k)) {
            return nullptr;
        }
        T* p = d + n;
        n += k;
        return p;
    }
    void Free() {
        free(static_cast<void*>(d));
        d = nullptr;
        n = cap = 0;
    }
};

struct RetSrc {
    const uint8_t* addr;
    uint32_t n;
    uint8_t cls;
};

struct RetImp {
    int16_t slot;
    uint8_t unit;
    uint32_t texId;
    TexKey key;
};

/* An import that samples a span an EARLIER run stored (the HUD loads a texture, calls a setup
 * list, then draws: two runs). Its key is a function of that span's descriptor and of the
 * source bytes its TMEM still owes, so both are part of the fingerprint. */
struct RetForeign {
    uint16_t word, base;
    uint16_t spanWords;
    uint8_t kind;
    uint8_t native, cls;
    uint32_t off, span;
    const uint8_t* src;
    LoadedTexture lt;
};

/* One rectangle of a recorded run, with everything its two triangles' floats (and, when its
 * first triangle began a batch, that batch's aux record) were computed from -- so a run that
 * differs from its record ONLY in rectangle geometry (positions, texture coordinates, fill
 * colours) can recompute exactly those floats and replay the rest (the variant hit). */
struct RetRect {
    uint16_t word;
    uint8_t op;
    uint8_t ok;       /* both triangles emitted, contiguous in one batch this run wrote */
    uint8_t began;    /* the first triangle began its batch (the aux is this rect's vertex 0) */
    uint8_t trectOk;  /* mTrectRectAtlasOk as the rectangle computed it */
    uint16_t serial;  /* the batch (0 = the one open at the run's entry) */
    uint16_t tri0;    /* the first triangle's index in that batch */
    uint32_t omL, omH, xgeom;
    DrawState ds;
    Gdx3dsVboPackParams params;
    Tile tiles[2]; /* texture_tile[ds.effective_tile[0 / 1]] */
    Rgba prim, env, fog, blend, keyC, keyS;
    uint8_t primLod;
    int16_t conv[6];
    LoadedVertex lv[4];
};

/* A colour / prim-depth write that met a batch opened before the run: its only use of the entry
 * value is whether the new value EQUALS it (NoteRegChange / the flush tests), so that answer is
 * checked instead of the value. */
struct RetPred {
    uint16_t word;
    uint8_t op;
    uint8_t equal;
};

/* Why a run was not retained ([hudr] miss / unretainable histograms). */
enum RetMiss {
    RM_NONE = 0,
    RM_EMPTY,   /* no record for this run slot */
    RM_WORDS,   /* the words differ */
    RM_REGS,    /* an entry register group differs (the group histogram says which) */
    RM_EXT,     /* frame config / levers / classification or atlas epoch */
    RM_TEX,     /* a cache slot no longer holds the recorded texture */
    RM_SRC,     /* a texture source moved since the record (DMA / native generation) */
    RM_HEAD,    /* VBO pool headroom */
    RM_COUNT
};
const char* const kMissName[RM_COUNT] = { "none", "empty", "words", "regs", "ext", "tex", "src", "head" };

enum RetUnret {
    RU_OP = 0,  /* an op retention does not model (triangles, LOADTILE, LOADTLUT, ...) */
    RU_REJECT,  /* a rectangle that drew fewer than two triangles */
    RU_LOAD,    /* a load that did not store a debt from a staleness-tested, non-immutable source */
    RU_IMPORT,  /* an import that missed, sampled a span not stored by this run, or is CI/RGBA32 */
    RU_BACKEND, /* an upload, a new shader, a refused/capped begin, a legacy draw */
    RU_COMB,    /* a new combiner */
    RU_ALLOC,
    RU_STATE,   /* a texskip window / trimemo / the verify oracle */
    RU_BIG,     /* longer than kMaxRetainWords */
    RU_COUNT
};
const char* const kUnretName[RU_COUNT] = { "op", "reject", "load", "import", "backend", "comb", "alloc", "state", "big" };

/* The TMEM span model, whole (retention verify only). */
struct SpanSnap {
    LoadedTexture lt[512];
    uint16_t base[512];
    uint16_t words[512];
    uint8_t kind[512];
    uint64_t stamp[512][2];
    uint8_t cls[512];
    uint16_t list[64];
    uint16_t listN;
    uint32_t tmemGen;
    uint32_t spanEpoch;
};

bool SameBytes(const void* a, const void* b, size_t n) {
    return memcmp(a, b, n) == 0;
}

bool SameRgba(const Rgba& a, const Rgba& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool SameRect(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool SameTileSet(const Tile& a, const Tile& b) {
    return a.fmt == b.fmt && a.siz == b.siz && a.cms == b.cms && a.cmt == b.cmt && a.masks == b.masks &&
           a.maskt == b.maskt && a.shifts == b.shifts && a.shiftt == b.shiftt && a.tmem == b.tmem &&
           a.line_size_bytes == b.line_size_bytes && a.palette == b.palette && a.tmem_index == b.tmem_index;
}

bool SameTileSize(const Tile& a, const Tile& b) {
    return SameBytes(&a.uls, &b.uls, sizeof(float)) && SameBytes(&a.ult, &b.ult, sizeof(float)) &&
           SameBytes(&a.lrs, &b.lrs, sizeof(float)) && SameBytes(&a.lrt, &b.lrt, sizeof(float));
}

bool SameRs0(const RenderState& a, const RenderState& b) {
    return a.depth_test_and_mask == b.depth_test_and_mask && a.decal_mode == b.decal_mode &&
           a.alpha_blend == b.alpha_blend && SameRect(a.viewport, b.viewport) && SameRect(a.scissor, b.scissor) &&
           a.shader_program == b.shader_program && a.textures[0] == b.textures[0] &&
           a.sampler_valid[0] == b.sampler_valid[0] && a.sampler_linear[0] == b.sampler_linear[0] &&
           a.sampler_cms[0] == b.sampler_cms[0] && a.sampler_cmt[0] == b.sampler_cmt[0];
}

bool SameRs1(const RenderState& a, const RenderState& b) {
    return a.textures[1] == b.textures[1] && a.sampler_valid[1] == b.sampler_valid[1] &&
           a.sampler_linear[1] == b.sampler_linear[1] && a.sampler_cms[1] == b.sampler_cms[1] &&
           a.sampler_cmt[1] == b.sampler_cmt[1];
}

bool SameParams(const Gdx3dsVboPackParams& a, const Gdx3dsVboPackParams& b) {
    return SameBytes(a.uScale, b.uScale, sizeof(a.uScale)) && SameBytes(a.vScale, b.vScale, sizeof(a.vScale)) &&
           SameBytes(a.wrapClampU, b.wrapClampU, sizeof(a.wrapClampU)) &&
           SameBytes(a.wrapClampV, b.wrapClampV, sizeof(a.wrapClampV)) && a.shaderClampGate == b.shaderClampGate &&
           a.used[0] == b.used[0] && a.used[1] == b.used[1] && a.fogFactorToAlpha == b.fogFactorToAlpha &&
           a.wantVtxAlpha == b.wantVtxAlpha && a.vtxColorInput == b.vtxColorInput &&
           a.vtxAlphaInput == b.vtxAlphaInput && SameBytes(a.uOff, b.uOff, sizeof(a.uOff)) &&
           SameBytes(a.vOff, b.vOff, sizeof(a.vOff)) && a.hasOff == b.hasOff;
}

/* The aux record as the backend reads it: numInputs entries of inputRgba, and the fog fields. */
bool SameAux(const Gdx3dsVboPackAux& a, const Gdx3dsVboPackAux& b) {
    const uint32_t n = a.numInputs < 7 ? a.numInputs : 7;
    return a.numInputs == b.numInputs && a.hasFog == b.hasFog && SameBytes(a.fogRgb, b.fogRgb, sizeof(a.fogRgb)) &&
           SameBytes(&a.fogFactor0, &b.fogFactor0, sizeof(float)) &&
           SameBytes(a.inputRgba, b.inputRgba, sizeof(a.inputRgba[0]) * n);
}

bool SameDrawState(const DrawState& a, const DrawState& b) {
    return a.comb == b.comb && a.prg == b.prg && a.tm == b.tm && SameBytes(a.tex_width, b.tex_width, sizeof(a.tex_width)) &&
           SameBytes(a.tex_height, b.tex_height, sizeof(a.tex_height)) &&
           SameBytes(a.tex_width2, b.tex_width2, sizeof(a.tex_width2)) &&
           SameBytes(a.tex_height2, b.tex_height2, sizeof(a.tex_height2)) &&
           SameBytes(a.effective_tile, b.effective_tile, sizeof(a.effective_tile)) &&
           a.usedTextures[0] == b.usedTextures[0] && a.usedTextures[1] == b.usedTextures[1] &&
           a.numInputs == b.numInputs && a.clip.zIsFrom0To1 == b.clip.zIsFrom0To1 &&
           a.clip.invertY == b.clip.invertY && a.use_alpha == b.use_alpha && a.use_fog == b.use_fog &&
           a.use_blend_color == b.use_blend_color;
}

bool SameEv(const RetEv& a, const RetEv& b) {
    return a.op == b.op && a.a == b.a && a.b == b.b && a.c == b.c && a.i[0] == b.i[0] && a.i[1] == b.i[1] &&
           a.i[2] == b.i[2] && a.i[3] == b.i[3] && SameBytes(&a.f, &b.f, sizeof(float)) && a.p == b.p;
}

} // namespace

/* ---- the register view -------------------------------------------------------------------- */

struct Interp::HudRegs {
    uint32_t omL, omH;
    uint64_t cc;
    Rgba prim;
    uint8_t primLod;
    Rgba env, fog, blend, fill;
    uint16_t pdepth;
    Rgba keyC, keyS;
    int16_t conv[6];
    Rect scis, vp;
    bool vsc;
    bool gray;
    Rgba grayCol;
    uint8_t fti;
    bool txc[2];
    struct {
        const uint8_t* addr;
        uint8_t fmt, siz;
        uint32_t width;
        uint8_t immutable, native;
        Cmd cmd;
    } ttl;
    uint32_t geom, xgeom;
    uint16_t tsS, tsT;
    int16_t fogMul, fogOff;
    bool zeq; /* color_image == z_buf: the one fact about the two images a FILLRECT reads */
    RenderState rs;
    size_t bufNumTris;
    bool packBatch;
    size_t packCap;
    Gdx3dsVboPackParams params;
    Gdx3dsVboPackAux aux;
    const Combiner* packComb;
    bool packFolded;
    LoadedVertex lv[4];
    DrawState triMemo;
    bool triMemoValid;
    Tile tiles[8];
};

/* One register group of the live state into `r` (the fingerprint captures only the groups it
 * compares; the record and verify capture them all). */
void Interp::HudRegCaptureGroup(HudRegs& r, int g) const {
    if (g >= RG_TILEF0) {
        const int t = (g - RG_TILEF0) & 7;
        r.tiles[t] = mRdp.texture_tile[t];
        return;
    }
    switch (g) {
        case RG_OM:
            r.omL = mRdp.other_mode_l;
            r.omH = mRdp.other_mode_h;
            break;
        case RG_CC: r.cc = mRdp.combine_mode; break;
        case RG_PRIM:
            r.prim = mRdp.prim_color;
            r.primLod = mRdp.prim_lod_fraction;
            break;
        case RG_ENV: r.env = mRdp.env_color; break;
        case RG_FOG: r.fog = mRdp.fog_color; break;
        case RG_BLEND: r.blend = mRdp.blend_color; break;
        case RG_FILL: r.fill = mRdp.fill_color; break;
        case RG_PDEPTH: r.pdepth = mRdp.prim_depth; break;
        case RG_KEY:
            r.keyC = mRdp.key_center;
            r.keyS = mRdp.key_scale;
            break;
        case RG_CONV: memcpy(r.conv, mRdp.convert_k, sizeof(r.conv)); break;
        case RG_SCIS: r.scis = mRdp.scissor; break;
        case RG_VP: r.vp = mRdp.viewport; break;
        case RG_VSC: r.vsc = mRdp.viewport_or_scissor_changed; break;
        case RG_GRAY:
            r.gray = mRdp.grayscale;
            r.grayCol = mRdp.grayscale_color;
            break;
        case RG_FTI: r.fti = mRdp.first_tile_index; break;
        case RG_TXC:
            r.txc[0] = mRdp.textures_changed[0];
            r.txc[1] = mRdp.textures_changed[1];
            break;
        case RG_TTL:
            r.ttl.addr = mRdp.texture_to_load.addr;
            r.ttl.fmt = mRdp.texture_to_load.fmt;
            r.ttl.siz = mRdp.texture_to_load.siz;
            r.ttl.width = mRdp.texture_to_load.width;
            r.ttl.immutable = mRdp.texture_to_load.immutable;
            r.ttl.native = mRdp.texture_to_load.native_rgba16;
            r.ttl.cmd = mRdp.texture_to_load.cmd;
            break;
        case RG_GEOM: r.geom = mRsp.geometry_mode; break;
        case RG_XGEOM: r.xgeom = mRsp.extra_geometry_mode; break;
        case RG_TSCALE:
            r.tsS = mRsp.texture_scaling_factor.s;
            r.tsT = mRsp.texture_scaling_factor.t;
            break;
        case RG_FOGMW:
            r.fogMul = mRsp.fog_mul;
            r.fogOff = mRsp.fog_offset;
            break;
        case RG_ZCI: r.zeq = mRdp.color_image_address == mRdp.z_buf_address; break;
        case RG_RS0:
        case RG_RS1: r.rs = mRs; break;
        case RG_PACK:
            r.bufNumTris = mBufNumTris;
            r.packBatch = mPackBatch;
            r.packCap = mPackCapTris;
            r.packComb = mPackComb;
            r.packFolded = mPackFolded;
            if (mBufNumTris > 0) { /* closed, the params/aux are not part of the state (HudRegSame) */
                r.params = mPackParams;
                r.aux = mPackAux;
            }
            break;
        case RG_LVXY:
        case RG_LVUV:
        case RG_LVCOL:
        case RG_LVCLIP: memcpy(r.lv, &mRsp.loaded_vertices[kMaxVertices], sizeof(r.lv)); break;
        case RG_TRIMEMO:
            r.triMemo = mTriMemo;
            r.triMemoValid = mTriMemoValid;
            break;
        default: break;
    }
}

void Interp::HudRegCapture(HudRegs& r) const {
    for (int g = 0; g < RG_TILES0; g++) {
        HudRegCaptureGroup(r, g);
    }
    r.params = mPackParams; /* the record keeps them whatever the batch (the exit's are applied) */
    r.aux = mPackAux;
}

bool Interp::HudRegSame(const HudRegs& a, const HudRegs& b, int g) const {
    if (g >= RG_TILES0) {
        return SameTileSize(a.tiles[g - RG_TILES0], b.tiles[g - RG_TILES0]);
    }
    if (g >= RG_TILEF0) {
        return SameTileSet(a.tiles[g - RG_TILEF0], b.tiles[g - RG_TILEF0]);
    }
    switch (g) {
        case RG_OM: return a.omL == b.omL && a.omH == b.omH;
        case RG_CC: return a.cc == b.cc;
        case RG_PRIM: return SameRgba(a.prim, b.prim) && a.primLod == b.primLod;
        case RG_ENV: return SameRgba(a.env, b.env);
        case RG_FOG: return SameRgba(a.fog, b.fog);
        case RG_BLEND: return SameRgba(a.blend, b.blend);
        case RG_FILL: return SameRgba(a.fill, b.fill);
        case RG_PDEPTH: return a.pdepth == b.pdepth;
        case RG_KEY: return SameRgba(a.keyC, b.keyC) && SameRgba(a.keyS, b.keyS);
        case RG_CONV: return SameBytes(a.conv, b.conv, sizeof(a.conv));
        case RG_SCIS: return SameRect(a.scis, b.scis);
        case RG_VP: return SameRect(a.vp, b.vp);
        case RG_VSC: return a.vsc == b.vsc;
        case RG_GRAY: return a.gray == b.gray && SameRgba(a.grayCol, b.grayCol);
        case RG_FTI: return a.fti == b.fti;
        case RG_TXC: return a.txc[0] == b.txc[0] && a.txc[1] == b.txc[1];
        case RG_TTL:
            return a.ttl.addr == b.ttl.addr && a.ttl.fmt == b.ttl.fmt && a.ttl.siz == b.ttl.siz &&
                   a.ttl.width == b.ttl.width && a.ttl.immutable == b.ttl.immutable && a.ttl.native == b.ttl.native &&
                   a.ttl.cmd.w0 == b.ttl.cmd.w0 && a.ttl.cmd.w1 == b.ttl.cmd.w1 && a.ttl.cmd.w1hi == b.ttl.cmd.w1hi;
        case RG_GEOM: return a.geom == b.geom;
        case RG_XGEOM: return a.xgeom == b.xgeom;
        case RG_TSCALE: return a.tsS == b.tsS && a.tsT == b.tsT;
        case RG_FOGMW: return a.fogMul == b.fogMul && a.fogOff == b.fogOff;
        case RG_ZCI: return a.zeq == b.zeq;
        case RG_RS0: return SameRs0(a.rs, b.rs);
        case RG_RS1: return SameRs1(a.rs, b.rs);
        case RG_PACK:
            if (a.bufNumTris != b.bufNumTris || a.packBatch != b.packBatch || a.packCap != b.packCap ||
                a.packComb != b.packComb || a.packFolded != b.packFolded) {
                return false;
            }
            /* Closed, the params/aux are stale and nothing reads them before the next begin
             * rewrites them; open, they are the batch. */
            return a.bufNumTris == 0 || (SameParams(a.params, b.params) && SameAux(a.aux, b.aux));
        case RG_LVXY:
        case RG_LVUV:
        case RG_LVCOL:
        case RG_LVCLIP:
            for (int i = 0; i < 4; i++) {
                const LoadedVertex& x = a.lv[i];
                const LoadedVertex& y = b.lv[i];
                bool same = true;
                if (g == RG_LVXY) {
                    same = SameBytes(&x.x, &y.x, 4 * sizeof(float));
                } else if (g == RG_LVUV) {
                    same = SameBytes(&x.u, &y.u, sizeof(float)) && SameBytes(&x.v, &y.v, sizeof(float));
                } else if (g == RG_LVCOL) {
                    same = SameRgba(x.color, y.color);
                } else {
                    same = x.clip_rej == y.clip_rej && x.clip_out == y.clip_out;
                }
                if (!same) {
                    return false;
                }
            }
            return true;
        case RG_TRIMEMO:
            return a.triMemoValid == b.triMemoValid && SameDrawState(a.triMemo, b.triMemo);
        default: return false;
    }
}

/* HudRegSame(live, b, g) without capturing the live group first (the fingerprint's compare). */
bool Interp::HudRegSameLive(const HudRegs& b, int g) const {
    if (g >= RG_TILES0) {
        return SameTileSize(mRdp.texture_tile[g - RG_TILES0], b.tiles[g - RG_TILES0]);
    }
    if (g >= RG_TILEF0) {
        return SameTileSet(mRdp.texture_tile[g - RG_TILEF0], b.tiles[g - RG_TILEF0]);
    }
    switch (g) {
        case RG_OM: return mRdp.other_mode_l == b.omL && mRdp.other_mode_h == b.omH;
        case RG_CC: return mRdp.combine_mode == b.cc;
        case RG_PRIM: return SameRgba(mRdp.prim_color, b.prim) && mRdp.prim_lod_fraction == b.primLod;
        case RG_ENV: return SameRgba(mRdp.env_color, b.env);
        case RG_FOG: return SameRgba(mRdp.fog_color, b.fog);
        case RG_BLEND: return SameRgba(mRdp.blend_color, b.blend);
        case RG_FILL: return SameRgba(mRdp.fill_color, b.fill);
        case RG_PDEPTH: return mRdp.prim_depth == b.pdepth;
        case RG_KEY: return SameRgba(mRdp.key_center, b.keyC) && SameRgba(mRdp.key_scale, b.keyS);
        case RG_CONV: return SameBytes(mRdp.convert_k, b.conv, sizeof(b.conv));
        case RG_SCIS: return SameRect(mRdp.scissor, b.scis);
        case RG_VP: return SameRect(mRdp.viewport, b.vp);
        case RG_VSC: return mRdp.viewport_or_scissor_changed == b.vsc;
        case RG_GRAY: return mRdp.grayscale == b.gray && SameRgba(mRdp.grayscale_color, b.grayCol);
        case RG_FTI: return mRdp.first_tile_index == b.fti;
        case RG_TXC: return mRdp.textures_changed[0] == b.txc[0] && mRdp.textures_changed[1] == b.txc[1];
        case RG_TTL: {
            const auto& t = mRdp.texture_to_load;
            return t.addr == b.ttl.addr && t.fmt == b.ttl.fmt && t.siz == b.ttl.siz && t.width == b.ttl.width &&
                   t.immutable == b.ttl.immutable && t.native_rgba16 == b.ttl.native && t.cmd.w0 == b.ttl.cmd.w0 &&
                   t.cmd.w1 == b.ttl.cmd.w1 && t.cmd.w1hi == b.ttl.cmd.w1hi;
        }
        case RG_GEOM: return mRsp.geometry_mode == b.geom;
        case RG_XGEOM: return mRsp.extra_geometry_mode == b.xgeom;
        case RG_TSCALE: return mRsp.texture_scaling_factor.s == b.tsS && mRsp.texture_scaling_factor.t == b.tsT;
        case RG_FOGMW: return mRsp.fog_mul == b.fogMul && mRsp.fog_offset == b.fogOff;
        case RG_ZCI: return (mRdp.color_image_address == mRdp.z_buf_address) == b.zeq;
        case RG_RS0: return SameRs0(mRs, b.rs);
        case RG_RS1: return SameRs1(mRs, b.rs);
        case RG_PACK:
            if (mBufNumTris != b.bufNumTris || mPackBatch != b.packBatch || mPackCapTris != b.packCap ||
                mPackComb != b.packComb || mPackFolded != b.packFolded) {
                return false;
            }
            return mBufNumTris == 0 || (SameParams(mPackParams, b.params) && SameAux(mPackAux, b.aux));
        case RG_LVXY:
        case RG_LVUV:
        case RG_LVCOL:
        case RG_LVCLIP:
            for (int i = 0; i < 4; i++) {
                const LoadedVertex& x = mRsp.loaded_vertices[kMaxVertices + i];
                const LoadedVertex& y = b.lv[i];
                bool same = true;
                if (g == RG_LVXY) {
                    same = SameBytes(&x.x, &y.x, 4 * sizeof(float));
                } else if (g == RG_LVUV) {
                    same = SameBytes(&x.u, &y.u, sizeof(float)) && SameBytes(&x.v, &y.v, sizeof(float));
                } else if (g == RG_LVCOL) {
                    same = SameRgba(x.color, y.color);
                } else {
                    same = x.clip_rej == y.clip_rej && x.clip_out == y.clip_out;
                }
                if (!same) {
                    return false;
                }
            }
            return true;
        case RG_TRIMEMO: return mTriMemoValid == b.triMemoValid && SameDrawState(mTriMemo, b.triMemo);
        default: return false;
    }
}

void Interp::HudRegApply(const HudRegs& r, int g) {
    if (g >= RG_TILES0) {
        Tile& t = mRdp.texture_tile[g - RG_TILES0];
        const Tile& s = r.tiles[g - RG_TILES0];
        t.uls = s.uls;
        t.ult = s.ult;
        t.lrs = s.lrs;
        t.lrt = s.lrt;
        return;
    }
    if (g >= RG_TILEF0) {
        Tile& t = mRdp.texture_tile[g - RG_TILEF0];
        const Tile& s = r.tiles[g - RG_TILEF0];
        t.fmt = s.fmt;
        t.siz = s.siz;
        t.cms = s.cms;
        t.cmt = s.cmt;
        t.masks = s.masks;
        t.maskt = s.maskt;
        t.shifts = s.shifts;
        t.shiftt = s.shiftt;
        t.tmem = s.tmem;
        t.line_size_bytes = s.line_size_bytes;
        t.palette = s.palette;
        t.tmem_index = s.tmem_index;
        return;
    }
    switch (g) {
        case RG_OM:
            mRdp.other_mode_l = r.omL;
            mRdp.other_mode_h = r.omH;
            break;
        case RG_CC: mRdp.combine_mode = r.cc; break;
        case RG_PRIM:
            mRdp.prim_color = r.prim;
            mRdp.prim_lod_fraction = r.primLod;
            break;
        case RG_ENV: mRdp.env_color = r.env; break;
        case RG_FOG: mRdp.fog_color = r.fog; break;
        case RG_BLEND: mRdp.blend_color = r.blend; break;
        case RG_FILL: mRdp.fill_color = r.fill; break;
        case RG_PDEPTH: mRdp.prim_depth = r.pdepth; break;
        case RG_KEY:
            mRdp.key_center = r.keyC;
            mRdp.key_scale = r.keyS;
            break;
        case RG_CONV: memcpy(mRdp.convert_k, r.conv, sizeof(r.conv)); break;
        case RG_SCIS: mRdp.scissor = r.scis; break;
        case RG_VP: mRdp.viewport = r.vp; break;
        case RG_VSC: mRdp.viewport_or_scissor_changed = r.vsc; break;
        case RG_GRAY:
            mRdp.grayscale = r.gray;
            mRdp.grayscale_color = r.grayCol;
            break;
        case RG_FTI: mRdp.first_tile_index = r.fti; break;
        case RG_TXC:
            mRdp.textures_changed[0] = r.txc[0];
            mRdp.textures_changed[1] = r.txc[1];
            break;
        case RG_TTL:
            mRdp.texture_to_load.addr = r.ttl.addr;
            mRdp.texture_to_load.fmt = r.ttl.fmt;
            mRdp.texture_to_load.siz = r.ttl.siz;
            mRdp.texture_to_load.width = r.ttl.width;
            mRdp.texture_to_load.immutable = r.ttl.immutable;
            mRdp.texture_to_load.native_rgba16 = r.ttl.native;
            mRdp.texture_to_load.cmd = r.ttl.cmd;
            break;
        case RG_GEOM: mRsp.geometry_mode = r.geom; break;
        case RG_XGEOM: mRsp.extra_geometry_mode = r.xgeom; break;
        case RG_TSCALE:
            mRsp.texture_scaling_factor.s = r.tsS;
            mRsp.texture_scaling_factor.t = r.tsT;
            break;
        case RG_FOGMW:
            mRsp.fog_mul = r.fogMul;
            mRsp.fog_offset = r.fogOff;
            break;
        case RG_ZCI: /* never written by a run (SETCIMG / SETZIMG are not recordable) */
            break;
        case RG_RS0: {
            const int16_t t1 = mRs.textures[1];
            const bool v1 = mRs.sampler_valid[1], l1 = mRs.sampler_linear[1];
            const uint8_t s1 = mRs.sampler_cms[1], u1 = mRs.sampler_cmt[1];
            mRs = r.rs;
            mRs.textures[1] = t1;
            mRs.sampler_valid[1] = v1;
            mRs.sampler_linear[1] = l1;
            mRs.sampler_cms[1] = s1;
            mRs.sampler_cmt[1] = u1;
            break;
        }
        case RG_RS1:
            mRs.textures[1] = r.rs.textures[1];
            mRs.sampler_valid[1] = r.rs.sampler_valid[1];
            mRs.sampler_linear[1] = r.rs.sampler_linear[1];
            mRs.sampler_cms[1] = r.rs.sampler_cms[1];
            mRs.sampler_cmt[1] = r.rs.sampler_cmt[1];
            break;
        case RG_PACK: /* HudRetainReplay applies the pack state itself (the pool cursor is live) */
            break;
        case RG_LVXY:
        case RG_LVUV:
        case RG_LVCOL:
        case RG_LVCLIP:
            for (int i = 0; i < 4; i++) {
                LoadedVertex& x = mRsp.loaded_vertices[kMaxVertices + i];
                const LoadedVertex& y = r.lv[i];
                if (g == RG_LVXY) {
                    x.x = y.x;
                    x.y = y.y;
                    x.z = y.z;
                    x.w = y.w;
                } else if (g == RG_LVUV) {
                    x.u = y.u;
                    x.v = y.v;
                } else if (g == RG_LVCOL) {
                    x.color = y.color;
                } else {
                    x.clip_rej = y.clip_rej;
                    x.clip_out = y.clip_out;
                }
            }
            break;
        case RG_TRIMEMO:
            mTriMemo = r.triMemo;
            mTriMemoValid = r.triMemoValid;
            break;
        default: break;
    }
}

/* ---- one retained run ---------------------------------------------------------------------- */

struct Interp::HudRetRec {
    bool valid;         /* a complete, retainable record */
    uint32_t shape;     /* ShapeKey of its words: the table set it lives in and the variant class */
    bool structUnret;   /* these words hold an op retention does not model: never record them again */
    uint32_t nWords;
    PodVec<GdxHudWord> words;
    HudRegs entry, exit;
    uint64_t cmpMask;   /* entry groups the run's effects depend on */
    uint64_t writeMask; /* groups the run writes (applied from `exit`) */
    bool paramsWritten, auxWritten, began;
    /* the facts outside the register file */
    uint32_t timgEpoch;
    unsigned long atlasEpoch;
    bool texRoom;
    uint32_t entryTexId[2];
    GdxFrameConfig cfg;
    uint64_t srcStamp[2];
    PodVec<HudLoadForm> loads;
    PodVec<RetSrc> srcs; /* the loads' distinct stored sources (the fingerprint's generation checks) */
    PodVec<RetImp> imps;
    PodVec<RetForeign> foreign;
    PodVec<RetRect> rectCtx;
    PodVec<RetEv> evs;
    PodVec<float> floats;
    PodVec<Gdx3dsVboPackParams> params;
    PodVec<Gdx3dsVboPackAux> auxs;
    uint32_t tris;  /* triangles the run writes into the pool (headroom) */
    uint32_t draws; /* batches it drains */
    uint32_t rects;
    int16_t tailBatchBegan; /* the batch open at exit was begun by this run */
    /* per-op dynamic facts noted while recording (the analysis reads them) */
    PodVec<uint8_t> opOpen;  /* a batch was open before the op */
    PodVec<uint8_t> opTex;   /* after a draw: bit0 unit 0 used, bit1 unit 1 used, bit7 known */
    PodVec<uint8_t> opTile;  /* the draw's first tile */
    PodVec<uint8_t> opFill;  /* a FILLRECT's path: 0 drew, 1 nothing (degenerate / full-screen z), 2 depth clear */
    PodVec<uint8_t> opRegs;  /* after a draw: the colour registers its combiner reads (DrawRegs) */
    PodVec<uint8_t> opPred;  /* a colour write's test against the live register: 1 equal, 2 differs */
    PodVec<uint8_t> opFlush; /* the op drained a batch */
    PodVec<RetPred> preds;
    /* [native] hudsky_emit (gdx_hudsky_emit.inc): this record's compiled plan (allocated on first
     * compile, kept across re-recordings), valid until the record is cleared */
    void* hsx;
    bool hsxValid;
};

struct Interp::HudRetain {
    /* census */
    uint32_t taskSeq;
    uint32_t runInTask;
    uint32_t prevHash[kRunSlots];
    uint32_t prevLen[kRunSlots];
    uint32_t curHash[kRunSlots];
    uint32_t curLen[kRunSlots];
    uint32_t winTasks, winRuns, winSame, winWords;
    uint64_t runT0;
    uint32_t batches0, imports0, texHits0, tmemLoads0;
    /* retention */
    HudRetRec* rec[kRunSlots][2]; /* two ways per run slot (allocated on first use): a run that
                                     alternates between two shapes (the blinking minimap marker)
                                     keeps both */
    uint8_t mru[kRunSlots];
    HudRetRec* fresh; /* the record being built (swapped into rec[slot] when complete) */
    HudRetRec* cur;   /* rec[slot] of the run being recorded */
    uint32_t curSlot; /* the run's index in the task (receipts) */
    uint32_t curSet;  /* its record set (by shape) */
    uint32_t curWay;
    bool wantCtx;           /* record the rectangles' contexts (the run's geometry has been seen to move) */
    uint8_t missStreak[kRunSlots];
    uint32_t coolUntil[kRunSlots]; /* a slot that keeps changing is not recorded until this task */
    uint32_t wCool;
    bool verifying;   /* the run being recorded is a verify execution of a would-be hit */
    bool unret;
    int unretWhy;
    uint32_t unretAt; /* the word index the run became unretainable at */
    uint32_t unretDetail;
    uint32_t curWord;
    uint32_t unretLogged;
    void Unret(int why) {
        if (!unret) {
            unret = true;
            unretWhy = why;
            unretAt = curWord;
        }
    }
    uint32_t impEntered0;
    uint32_t impNoted;
    uint32_t combFlush0;
    uint32_t curStartTri; /* the first triangle of the open batch this run wrote */
    uint32_t curSerial;   /* batches begun so far by the run being recorded (0 = the entry batch) */
    uint32_t preSerial, preEmit, preBatches;
    uint32_t wVarHit, wVarRects, wVarSelfBad, wVarNo;
    uint32_t wVarWhy[8]; /* 0 fingerprint, 1 rect map, 2 fill path, 3 texrect tile/atlas, 4 unpatchable, 5 offsets, 6 self-check */
    uint8_t lastPath; /* the census: 0 executed, 1 replayed, 2 replayed as a variant */
    uint8_t lastWhy;
    uint8_t lastGroup; /* the first differing register group of the last regs miss */
    PodVec<float> varFloats;
    PodVec<Gdx3dsVboPackAux> varAux;
    PodVec<uint8_t> varDirty;
    uint64_t storedWords[8]; /* TMEM words (512 bits) the run's loads stored spans over */
    HudRegs vfyEntry;
    SpanSnap* vfySpanPred;
    SpanSnap* vfySpanTmp;
    uint8_t* vfyLedgerTmp;
    uint8_t vfyTmemPred[4096];
    uint8_t vfyTmemLive[4096];
    Backend* realBackend;
    HudRecBackend* recBackend;
    /* [hudr] window */
    uint32_t wRuns, wHit, wMiss[RM_COUNT], wUnret[RU_COUNT], wRecorded, wRegMiss[RG_COUNT];
    uint32_t wHitRects, wHitLoads, wHitImps, wHitDraws, wHitTris;
    uint32_t wVfyRuns, wVfyOk, wVfyBad, wVfyBadEv, wVfyBadFloat, wVfyBadReg, wVfySpan, wVfyTmem, wVfyLru, wVfyUnret;
    uint64_t wTicksHit, wTicksExec;
    /* [native] hudclock>=2: where retention's own time goes (per task) */
    uint64_t wTicksFp, wTicksVar, wTicksRecBegin, wTicksRecEnd, wTicksReplay;
    uint64_t wTicksRLoad, wTicksREv, wTicksRApply;
    uint64_t wTicksFpPh[4]; /* hudretclock=2: the fingerprint's regs / tex / plan / sources */
    uint32_t sesVfyBad, sesVfyRuns;
    uint32_t vfyLogged;
};

/* ---- the recording decorator ------------------------------------------------------------- */

class Interp::HudRecBackend final : public Backend {
  public:
    HudRecBackend(Interp* it) : mI(it) {
    }
    Backend* real = nullptr;

    ClipParams GetClipParameters() override {
        return real->GetClipParameters();
    }
    int GetMaxTextureSize() override {
        return real->GetMaxTextureSize();
    }
    void UnloadShader(void* prg) override {
        Ev(EV_UNLOAD, 0, 0, 0, prg);
        real->UnloadShader(prg);
    }
    void* LookupShader(uint64_t id0, uint64_t id1) override {
        return real->LookupShader(id0, id1);
    }
    void* CreateAndLoadNewShader(uint64_t id0, uint64_t id1) override {
        Unret(RU_BACKEND);
        return real->CreateAndLoadNewShader(id0, id1);
    }
    void LoadShader(void* prg) override {
        Ev(EV_LOAD, 0, 0, 0, prg);
        real->LoadShader(prg);
    }
    void ShaderGetInfo(void* prg, uint8_t* numInputs, bool used[2]) override {
        real->ShaderGetInfo(prg, numInputs, used);
    }
    uint32_t NewTexture() override {
        Unret(RU_BACKEND);
        return real->NewTexture();
    }
    void SelectTexture(int tile, uint32_t id) override {
        RetEv e = Base(EV_SELTEX);
        e.i[0] = tile;
        e.i[1] = static_cast<int32_t>(id);
        Push(e);
        real->SelectTexture(tile, id);
    }
    void UploadTexture(const uint8_t* rgba32, uint32_t w, uint32_t h) override {
        Unret(RU_BACKEND);
        real->UploadTexture(rgba32, w, h);
    }
    void SetSamplerParameters(int sampler, bool linear, uint32_t cms, uint32_t cmt) override {
        RetEv e = Base(EV_SAMPLER);
        e.i[0] = sampler;
        e.a = linear ? 1 : 0;
        e.i[1] = static_cast<int32_t>(cms);
        e.i[2] = static_cast<int32_t>(cmt);
        Push(e);
        real->SetSamplerParameters(sampler, linear, cms, cmt);
    }
    void DeleteTexture(uint32_t id) override {
        Unret(RU_BACKEND);
        real->DeleteTexture(id);
    }
    int GetTextureFilter() override {
        return real->GetTextureFilter();
    }
    void SetDepthTestAndMask(bool t, bool m) override {
        Ev(EV_DEPTH, t ? 1 : 0, m ? 1 : 0, 0, nullptr);
        real->SetDepthTestAndMask(t, m);
    }
    void SetZmodeDecal(bool d) override {
        Ev(EV_DECAL, d ? 1 : 0, 0, 0, nullptr);
        real->SetZmodeDecal(d);
    }
    void SetViewport(int x, int y, int w, int h) override {
        Box(EV_VIEWPORT, x, y, w, h);
        real->SetViewport(x, y, w, h);
    }
    void SetScissor(int x, int y, int w, int h) override {
        Box(EV_SCISSOR, x, y, w, h);
        real->SetScissor(x, y, w, h);
    }
    void SetUseAlpha(bool a) override {
        Ev(EV_ALPHA, a ? 1 : 0, 0, 0, nullptr);
        real->SetUseAlpha(a);
    }
    void SetCurrentPrimDepth(float d) override {
        RetEv e = Base(EV_PRIMDEPTH);
        e.f = d;
        e.i[3] = static_cast<int32_t>(mI->mHudR->curWord); /* the op that drained */
        Push(e);
        real->SetCurrentPrimDepth(d);
    }
    void SetCurrentAlphaCompareThreshold(float t) override {
        RetEv e = Base(EV_ALPHATHR);
        e.f = t;
        e.i[3] = static_cast<int32_t>(mI->mHudR->curWord);
        Push(e);
        real->SetCurrentAlphaCompareThreshold(t);
    }
    void DrawTriangles(float buf[], size_t len, size_t tris) override {
        Unret(RU_BACKEND);
        real->DrawTriangles(buf, len, tris);
    }
    float* VboPackBegin(Gdx3dsVboPackParams* p, uint32_t* capTris) override {
        float* dst = real->VboPackBegin(p, capTris);
        uint32_t cap = capTris != nullptr ? *capTris : 0u;
        if (dst == nullptr || cap < static_cast<uint32_t>(kMaxTriBuffer)) {
            Unret(RU_BACKEND); /* a refused or capped begin depends on the pool's state */
        }
        /* The interpreter takes min(cap, kMaxTriBuffer): past that the pool's size is no input. */
        cap = cap < static_cast<uint32_t>(kMaxTriBuffer) ? cap : static_cast<uint32_t>(kMaxTriBuffer);
        HudRetRec& r = *mI->mHudR->fresh;
        RetEv e = Base(EV_BEGIN);
        e.i[0] = static_cast<int32_t>(r.params.n);
        e.i[1] = static_cast<int32_t>(cap);
        if (!r.params.Push(*p)) {
            Unret(RU_ALLOC);
        }
        Push(e);
        r.began = true;
        r.paramsWritten = true;
        r.auxWritten = true;
        mI->mHudR->curStartTri = 0;
        mI->mHudR->curSerial++;
        return dst;
    }
    void DrawPackedBatch(size_t tris, const Gdx3dsVboPackAux* aux) override {
        CaptureFloats(tris);
        HudRetRec& r = *mI->mHudR->fresh;
        RetEv e = Base(EV_DRAW);
        e.i[0] = static_cast<int32_t>(tris);
        e.i[1] = static_cast<int32_t>(r.auxs.n);
        e.i[2] = static_cast<int32_t>(mI->mHudR->curSerial);
        if (aux == nullptr || !r.auxs.Push(*aux)) {
            Unret(RU_ALLOC);
        }
        Push(e);
        r.draws++;
        real->DrawPackedBatch(tris, aux);
    }
    void StartFrame() override {
        Unret(RU_BACKEND);
        real->StartFrame();
    }
    void UpdateFramebufferParameters(int fb, uint32_t w, uint32_t h, uint32_t msaa, bool invY, bool rt, bool depth,
                                     bool extract) override {
        Unret(RU_BACKEND);
        real->UpdateFramebufferParameters(fb, w, h, msaa, invY, rt, depth, extract);
    }
    void StartDrawToFramebuffer(int fb, float noise) override {
        Unret(RU_BACKEND);
        real->StartDrawToFramebuffer(fb, noise);
    }
    void ClearFramebuffer(bool c, bool d) override {
        Unret(RU_BACKEND);
        real->ClearFramebuffer(c, d);
    }
    void ClearDepthRegion(int x, int y, int w, int h) override {
        Box(EV_CLEARDEPTH, x, y, w, h);
        real->ClearDepthRegion(x, y, w, h);
    }
    void FogNoteParams(int mul, int off, int exact) override {
        RetEv e = Base(EV_FOGNOTE);
        e.i[0] = mul;
        e.i[1] = off;
        e.i[2] = exact;
        Push(e);
        real->FogNoteParams(mul, off, exact);
    }
    void* GetFramebufferTextureId(int fb) override {
        return real->GetFramebufferTextureId(fb);
    }
    void SelectTextureFb(int fb) override {
        Unret(RU_BACKEND);
        real->SelectTextureFb(fb);
    }
    bool ProfActive() override {
        return real->ProfActive();
    }
    uint64_t ProfEnter(uint64_t* snap) override {
        return real->ProfEnter(snap);
    }
    void ProfExit(int sec, uint64_t t0, uint64_t snap) override {
        real->ProfExit(sec, t0, snap);
    }
    bool VerifyActive() override {
        return real->VerifyActive();
    }
    void VerifyNoteTri(const void* tri) override {
        real->VerifyNoteTri(tri);
    }
    void VerifyNoteCmd(uint32_t w0, uint32_t w1) override {
        real->VerifyNoteCmd(w0, w1);
    }
    void TexCacheNoteImport() override {
        real->TexCacheNoteImport();
    }
    void TexCacheNoteMiss(const void* addr, unsigned fmt, unsigned siz, unsigned tmemWord, unsigned lineBytes,
                          unsigned sizeBytes, unsigned tileW, unsigned tileH, unsigned hash) override {
        real->TexCacheNoteMiss(addr, fmt, siz, tmemWord, lineBytes, sizeBytes, tileW, tileH, hash);
    }
    bool BatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask) override {
        return real->BatchConstInputs(rgbMask, alphaMask);
    }
    bool TrectBatchEnabled() override {
        return real->TrectBatchEnabled();
    }
    void AtlasArm(bool on) override {
        Ev(EV_ATLASARM, on ? 1 : 0, 0, 0, nullptr);
        real->AtlasArm(on);
    }
    int TexPage(uint32_t texId) override {
        return real->TexPage(texId);
    }
    uint32_t TexBytes(uint32_t texId) override {
        return real->TexBytes(texId);
    }
    size_t LinearFreeBytes() override {
        return real->LinearFreeBytes();
    }
    void VboPackRefresh(Gdx3dsVboPackParams* params) override {
        real->VboPackRefresh(params);
        HudRetRec& r = *mI->mHudR->fresh;
        RetEv e = Base(EV_REFRESH);
        e.i[0] = static_cast<int32_t>(r.params.n);
        if (!r.params.Push(*params)) {
            Unret(RU_ALLOC);
        }
        Push(e);
        r.paramsWritten = true;
    }
    size_t VboFreeFloats() override {
        return real->VboFreeFloats();
    }
    unsigned long AtlasEpoch() override {
        return real->AtlasEpoch();
    }
    const unsigned long* AtlasEpochPtr() override {
        return real->AtlasEpochPtr();
    }
    void AtlasResetAll() override {
        real->AtlasResetAll();
    }

    /* The triangles [curStartTri, tris) of the batch being drawn (or left open at the run's
     * end) were written by this run: keep their floats, in pool order. */
    void CaptureFloats(size_t tris) {
        HudRetain& R = *mI->mHudR;
        HudRetRec& r = *R.fresh;
        const uint32_t start = R.curStartTri;
        if (tris > start && mI->mPackDst != nullptr) {
            const uint32_t n = static_cast<uint32_t>(tris) - start;
            RetEv e = Base(EV_FLOATS);
            e.i[0] = static_cast<int32_t>(start);
            e.i[1] = static_cast<int32_t>(n);
            e.i[2] = static_cast<int32_t>(r.floats.n);
            e.i[3] = static_cast<int32_t>(R.curSerial);
            float* out = r.floats.Grow(n * static_cast<uint32_t>(kPackTriFloats));
            if (out == nullptr) {
                Unret(RU_ALLOC);
            } else {
                memcpy(out, mI->mPackDst + start * kPackTriFloats, n * kPackTriFloats * sizeof(float));
            }
            Push(e);
            r.tris += n;
        }
        R.curStartTri = 0;
    }

  private:
    Interp* mI;
    static RetEv Base(uint8_t op) {
        RetEv e;
        memset(&e, 0, sizeof(e));
        e.op = op;
        return e;
    }
    __attribute__((noinline)) void Push(const RetEv& e) {
        if (!mI->mHudR->fresh->evs.Push(e)) {
            Unret(RU_ALLOC);
        }
    }
    void Ev(uint8_t op, uint8_t a, uint8_t b, uint8_t c, const void* p) {
        RetEv e = Base(op);
        e.a = a;
        e.b = b;
        e.c = c;
        e.p = p;
        Push(e);
    }
    void Box(uint8_t op, int x, int y, int w, int h) {
        RetEv e = Base(op);
        e.i[0] = x;
        e.i[1] = y;
        e.i[2] = w;
        e.i[3] = h;
        Push(e);
    }
    void Unret(int why) {
        mI->mHudR->Unret(why);
    }
};

/* ---- lifetime ------------------------------------------------------------------------------ */

namespace {

void RecClear(Interp::HudRetRec& r);
Interp::HudRetRec* RecNew();

} // namespace

bool Interp::HudRetainEnsure() {
    if (mHudR != nullptr) {
        return true;
    }
    mHudR = new (std::nothrow) HudRetain;
    if (mHudR == nullptr) {
        mHudElem = 0;
        mHudRetain = 0;
        return false;
    }
    memset(static_cast<void*>(mHudR), 0, sizeof(HudRetain));
    mHudR->recBackend = new (std::nothrow) HudRecBackend(this);
    mHudR->fresh = RecNew();
    if (mHudR->recBackend == nullptr || mHudR->fresh == nullptr) {
        mHudRetain = 0;
    }
    return true;
}

namespace {

template <typename T>
void Reset(PodVec<T>& v) {
    v.n = 0;
}

void RecClear(Interp::HudRetRec& r) {
    r.valid = false;
    r.shape = 0;
    r.structUnret = false;
    r.nWords = 0;
    Reset(r.words);
    Reset(r.loads);
    Reset(r.srcs);
    Reset(r.imps);
    Reset(r.foreign);
    Reset(r.rectCtx);
    Reset(r.evs);
    Reset(r.floats);
    Reset(r.params);
    Reset(r.auxs);
    Reset(r.opOpen);
    Reset(r.opTex);
    Reset(r.opTile);
    Reset(r.opFill);
    Reset(r.opRegs);
    Reset(r.opPred);
    Reset(r.opFlush);
    Reset(r.preds);
    r.cmpMask = r.writeMask = 0;
    r.paramsWritten = r.auxWritten = r.began = false;
    r.tris = r.draws = r.rects = 0;
    r.tailBatchBegan = 0;
    r.hsxValid = false; /* [native] hudsky_emit: the plan follows the record */
}

/* A zeroed record (its buffers grow on first use); nullptr when the heap is out. */
Interp::HudRetRec* RecNew() {
    void* p = malloc(sizeof(Interp::HudRetRec));
    if (p != nullptr) {
        memset(p, 0, sizeof(Interp::HudRetRec));
    }
    return static_cast<Interp::HudRetRec*>(p);
}

} // namespace

void Interp::HudRetainClear() {
    if (mHudR == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < kRunSlots; i++) {
        for (int k = 0; k < 2; k++) {
            if (mHudR->rec[i][k] != nullptr) {
                mHudR->rec[i][k]->valid = false;
            }
        }
    }
}

/* ---- where a run's record lives ------------------------------------------------------------- */

namespace {

/* A run's SHAPE: its words with every rectangle's geometry and every fill colour masked out --
 * what a variant hit may change. Records are found by shape, not by the run's index in the task,
 * so an element that appears or disappears (the check marker behind the player) does not shift
 * every later run onto another run's record. */
uint32_t ShapeKey(const GdxHudWord* w, uint32_t n) {
    uint32_t h = 2166136261u ^ n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t op = Opcode(w[i].w0);
        if (op == OP_TEXRECT || op == OP_TEXRECTFLIP) {
            h = (h ^ (static_cast<uint32_t>(op) << 8 | C1(w[i].w1, 24, 3))) * 16777619u;
            i += 2;
        } else if (op == OP_FILLRECT || op == OP_SETFILLCOLOR) {
            h = (h ^ op) * 16777619u;
        } else {
            h = (h ^ w[i].w0) * 16777619u;
            h = (h ^ w[i].w1) * 16777619u;
            h = (h ^ w[i].w1hi) * 16777619u;
        }
    }
    return h;
}

} // namespace

/* ---- the census ---------------------------------------------------------------------------- */

namespace {

uint32_t WordsFnv(const GdxHudWord* w, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ w[i].w0) * 16777619u;
        h = (h ^ w[i].w1) * 16777619u;
        h = (h ^ w[i].w1hi) * 16777619u;
    }
    return h;
}

} // namespace

bool Interp::HudElemCensusTask() const {
    return mHudR != nullptr && mHudElem != 0 && (mHudR->taskSeq % kElemEvery) == 0;
}

void Interp::HudElemBegin() {
    if (!HudRetainEnsure()) {
        return;
    }
    HudRetain& R = *mHudR;
    R.runT0 = static_cast<uint64_t>(gdx_ninterp_now());
    R.batches0 = mStats.batches;
    R.imports0 = mStats.texImports;
    R.texHits0 = mStats.texHits;
    R.tmemLoads0 = mStats.tmemLoads;
}

/* One HUD run finished: the identity table, and on a census task its [hudelem] line. */
void Interp::HudElemEnd(const void* runWords, uint32_t n) {
    if (mHudR == nullptr) {
        return;
    }
    HudRetain& R = *mHudR;
    const uint64_t ticks = static_cast<uint64_t>(gdx_ninterp_now()) - R.runT0;
    const GdxHudWord* w = static_cast<const GdxHudWord*>(runWords);
    const uint32_t idx = R.runInTask;
    const uint32_t h = WordsFnv(w, n);
    bool same = false;
    if (idx < kRunSlots) {
        same = R.prevHash[idx] == h && R.prevLen[idx] == n;
        R.curHash[idx] = h;
        R.curLen[idx] = n;
    }
    R.winRuns++;
    R.winWords += n;
    if (same) {
        R.winSame++;
    }
    if ((R.taskSeq % kElemEvery) != 0) {
        return;
    }
    /* hotcold: the census line, out of line */
    [this, &R, w, n, idx, same, ticks]() GDX_COLD {
    uint32_t loads = 0, texrects = 0, fillrects = 0, colours = 0, tiles = 0, other = 0;
    int32_t x0 = 0x7FFFFFFF, y0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, y1 = -0x7FFFFFFF;
    char tex[200];
    int tn = 0;
    tex[0] = '\0';
    uint32_t timgFmt = 0, timgSiz = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t op = Opcode(w[i].w0);
        if (op == OP_TEXRECT || op == OP_TEXRECTFLIP || op == OP_FILLRECT) {
            const int32_t lrx = static_cast<int32_t>(C0(w[i].w0, 12, 12)) / 4;
            const int32_t lry = static_cast<int32_t>(C0(w[i].w0, 0, 12)) / 4;
            const int32_t ulx = static_cast<int32_t>(C1(w[i].w1, 12, 12)) / 4;
            const int32_t uly = static_cast<int32_t>(C1(w[i].w1, 0, 12)) / 4;
            x0 = ulx < x0 ? ulx : x0;
            y0 = uly < y0 ? uly : y0;
            x1 = lrx > x1 ? lrx : x1;
            y1 = lry > y1 ? lry : y1;
            if (op == OP_FILLRECT) {
                fillrects++;
            } else {
                texrects++;
                i += 2; /* the two RDPHALF trailers */
            }
        } else if (op == OP_LOADBLOCK || op == OP_LOADTILE || op == OP_LOADTLUT) {
            loads++;
            if (op == OP_LOADTLUT && tn < static_cast<int>(sizeof(tex)) - 16) {
                tn += snprintf(tex + tn, sizeof(tex) - static_cast<size_t>(tn), " tlut");
            }
        } else if (op == OP_SETTIMG) {
            timgFmt = C0(w[i].w0, 21, 3);
            timgSiz = C0(w[i].w0, 19, 2);
        } else if (op == OP_SETTILESIZE) {
            tiles++;
            if (C1(w[i].w1, 24, 3) == 0 && tn < static_cast<int>(sizeof(tex)) - 24) {
                const uint32_t tw = (C1(w[i].w1, 12, 12) - C0(w[i].w0, 12, 12)) / 4 + 1;
                const uint32_t th = (C1(w[i].w1, 0, 12) - C0(w[i].w0, 0, 12)) / 4 + 1;
                tn += snprintf(tex + tn, sizeof(tex) - static_cast<size_t>(tn), " %u/%u:%ux%u", timgFmt, timgSiz,
                               tw, th);
            }
        } else if (op == OP_SETPRIMCOLOR || op == OP_SETENVCOLOR || op == OP_SETFILLCOLOR) {
            colours++;
        } else if (op == OP_SETTILE) {
            tiles++;
        } else {
            other++;
        }
    }
    const double us = static_cast<double>(ticks) / 268.111856;
    static const char* const kPath[3] = { "exec", "hit", "variant" };
    GDXN_LOG("[hudelem] task=%u run=%u w=%u same=%d %s(%s) ld=%u tr=%u fr=%u col=%u tile=%u oth=%u box=%d,%d-%d,%d "
             "drn=%u imp=%u hit=%u tml=%u us=%.1f tex=%s",
             R.taskSeq, idx, n, same ? 1 : 0, mHudRetain != 0 ? kPath[R.lastPath % 3u] : "-",
             mHudRetain != 0 ? (R.lastWhy == RM_REGS ? (R.lastGroup == 0xFF ? "pred" : kGroupName[R.lastGroup % RG_COUNT])
                                                     : kMissName[R.lastWhy % RM_COUNT])
                             : "-",
             loads, texrects, fillrects, colours, tiles, other,
             texrects + fillrects != 0 ? x0 : 0, texrects + fillrects != 0 ? y0 : 0,
             texrects + fillrects != 0 ? x1 : 0, texrects + fillrects != 0 ? y1 : 0, mStats.batches - R.batches0,
             mStats.texImports - R.imports0, mStats.texHits - R.texHits0, mStats.tmemLoads - R.tmemLoads0, us,
             tn != 0 ? tex : " -");
    }();
}

/* ---- the analysis: which entry groups the run's effects depend on ------------------------- */

namespace {

/* Which colour registers a draw under `ds` reads (SpTri's gdxInput over the mapped inputs, the
 * aux record's, and the fog): bit0 prim (+ lod fraction), bit1 env, bit2 fog, bit3 key/convert.
 * The LOD-fraction input reads other_mode_l (always compared) and the vertex w. */
uint8_t DrawRegs(const DrawState& ds) {
    uint8_t m = ds.use_fog ? 4u : 0u;
    for (int k = 0; k < 2; k++) {
        for (int j = 0; j < 7; j++) {
            const uint8_t v = ds.comb->shader_input_mapping[k][j];
            if (v == CCMUX_PRIMITIVE || v == CCMUX_PRIMITIVE_ALPHA || v == CCMUX_PRIM_LOD_FRAC ||
                (k == 1 && v == ACMUX_PRIM_LOD_FRAC)) {
                m |= 1u;
            } else if (v == CCMUX_ENVIRONMENT || v == CCMUX_ENV_ALPHA) {
                m |= 2u;
            } else if (v == CCMUX_KEY_CENTER || v == CCMUX_KEY_SCALE || v == CCMUX_CONVERT_K4 ||
                       v == CCMUX_CONVERT_K5) {
                m |= 8u;
            }
        }
    }
    return m;
}

/* Does a draw under `ds` read its corners' colour? Only through a shade input of the combiner
 * (SpTri's gdxInput: SHADE / SHADE_ALPHA in the colour channel, SHADE in the alpha channel) or
 * the fog factor (vertex alpha when fog is on). */
bool DrawReadsVertexColour(const DrawState& ds) {
    if (ds.use_fog) {
        return true;
    }
    for (int j = 0; j < 7; j++) {
        const uint8_t c = ds.comb->shader_input_mapping[0][j];
        const uint8_t a = ds.comb->shader_input_mapping[1][j];
        if (c == CCMUX_SHADE || c == CCMUX_SHADE_ALPHA || a == ACMUX_SHADE) {
            return true;
        }
    }
    return false;
}

/* The groups a draw reads (everything its state block, emission and flushes read), for a
 * draw on `tile` whose combiner samples the units in `texMask` (bit0 / bit1). */
uint64_t DrawReads(uint8_t tile, uint8_t texMask, bool fill, bool vtxColour, bool blendRgb, uint8_t regs) {
    /* prim depth and blend alpha reach a draw only through the drains its state block makes:
     * those are read where the drain's event is (HudRetainAnalyse). The colour registers reach
     * it only through the combiner inputs that map them (and fog): `regs` says which. */
    uint64_t m = Bit(RG_OM) | Bit(RG_CC) | ((regs & 1u) ? Bit(RG_PRIM) : 0u) | ((regs & 2u) ? Bit(RG_ENV) : 0u) |
                 ((regs & 4u) ? Bit(RG_FOG) : 0u) | ((regs & 8u) ? (Bit(RG_KEY) | Bit(RG_CONV)) : 0u) |
                 (blendRgb ? Bit(RG_BLEND) : 0u) | Bit(RG_SCIS) | Bit(RG_VP) | Bit(RG_VSC) |
                 Bit(RG_GRAY) | Bit(RG_FTI) | Bit(RG_TXC) | Bit(RG_GEOM) | Bit(RG_XGEOM) | Bit(RG_FOGMW) |
                 Bit(RG_RS0) | Bit(RG_PACK) | Bit(RG_LVCLIP);
    if (vtxColour) {
        m |= Bit(RG_LVCOL); /* the corners' colour reaches the floats only through a shade input or fog */
    }
    if (fill) {
        m |= Bit(RG_ZCI) | Bit(RG_FILL);
    }
    if ((texMask & 1u) != 0) {
        m |= Bit(RG_TILEF0 + (tile & 7)) | Bit(RG_TILES0 + (tile & 7));
        if (fill) {
            m |= Bit(RG_LVUV); /* a FILLRECT does not write u, v: a textured one reads them */
        }
    }
    if ((texMask & 2u) != 0) {
        const int t1 = (tile + 1) & 7;
        m |= Bit(RG_RS1) | Bit(RG_TILEF0 + t1) | Bit(RG_TILES0 + t1);
        if (fill) {
            m |= Bit(RG_LVUV);
        }
    }
    return m;
}

uint64_t DrawWrites(uint8_t texMask, bool fill) {
    uint64_t m = Bit(RG_TXC) | Bit(RG_VSC) | Bit(RG_LVXY) | Bit(RG_RS0) | Bit(RG_PACK) | Bit(RG_TRIMEMO);
    m |= fill ? Bit(RG_LVCOL) : Bit(RG_LVUV);
    if ((texMask & 2u) != 0) {
        m |= Bit(RG_RS1);
    }
    return m;
}

struct Taint {
    uint64_t t[RG_COUNT];
    uint64_t cmp;
    uint64_t wr;
    void Init() {
        for (int g = 0; g < RG_COUNT; g++) {
            t[g] = Bit(g);
        }
        cmp = 0;
        wr = 0;
    }
    uint64_t Of(uint64_t groups) const {
        uint64_t m = 0;
        for (; groups != 0; groups &= groups - 1u) {
            m |= t[__builtin_ctzll(groups)];
        }
        return m;
    }
    /* An op whose effect leaves the interpreter: everything it reads is compared, and what it
     * writes is then a function of compared values. */
    void Observe(uint64_t reads, uint64_t writes) {
        cmp |= Of(reads);
        for (uint64_t w = writes; w != 0; w &= w - 1u) {
            t[__builtin_ctzll(w)] = 0;
        }
        wr |= writes;
    }
    void Full(int g) {
        t[g] = 0;
        wr |= Bit(g);
    }
    /* A partial or value-gated write: the old value (and `inputs`) stay in the result. */
    void Partial(int g, uint64_t inputs) {
        t[g] |= Of(inputs);
        wr |= Bit(g);
    }
};

} // namespace

/* The analysis over the recorded words and the per-op facts noted while they executed. Returns
 * false when the run holds an op retention does not model. */
static bool HudRetainAnalyse(Interp::HudRetRec& r, uint64_t* cmpOut, uint64_t* writeOut) {
    Taint T;
    T.Init();
    const GdxHudWord* w = r.words.d;
    const uint32_t n = r.nWords;
    uint32_t ev = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t op = Opcode(w[i].w0);
        const bool open = i < r.opOpen.n && r.opOpen.d[i] != 0;
        /* this op's drains: the prim depth / blend alpha they were drawn with is the register's
         * value when the op began (a drain precedes the op's own write). An entry value is
         * applied LIVE at replay (the event is marked), a run-written one is recorded, a mixed
         * one is compared. */
        for (; ev < r.evs.n && (r.evs.d[ev].op != EV_PRIMDEPTH && r.evs.d[ev].op != EV_ALPHATHR ?
                                    true : static_cast<uint32_t>(r.evs.d[ev].i[3]) <= i); ev++) {
            RetEv& e = r.evs.d[ev];
            if (e.op != EV_PRIMDEPTH && e.op != EV_ALPHATHR) {
                continue;
            }
            const int g = e.op == EV_PRIMDEPTH ? RG_PDEPTH : RG_BLEND;
            if (T.t[g] == Bit(g)) {
                e.c = 1;
            } else {
                e.c = 0;
                T.cmp |= T.t[g];
            }
        }
        switch (op) {
            case OP_SETOTHERMODE_L:
            case OP_SETOTHERMODE_H: T.Partial(RG_OM, 0); break;
            case OP_RDPSETOTHERMODE: T.Full(RG_OM); break;
            case OP_SETCOMBINE: T.Full(RG_CC); break;
            case OP_SETPRIMCOLOR:
            case OP_SETENVCOLOR:
            case OP_SETFOGCOLOR:
            case OP_SETBLENDCOLOR:
            case OP_SETPRIMDEPTH: {
                const int g = op == OP_SETPRIMCOLOR ? RG_PRIM
                              : op == OP_SETENVCOLOR ? RG_ENV
                              : op == OP_SETFOGCOLOR ? RG_FOG
                              : op == OP_SETBLENDCOLOR ? RG_BLEND
                                                       : RG_PDEPTH;
                if (open) {
                    /* with a batch open, a value CHANGE splits it (or folds into it): the entry
                     * value is used only through that equality -- a predicate the fingerprint
                     * checks -- unless the register was already written by the run */
                    if (T.t[g] == Bit(g)) {
                        const RetPred pr = { static_cast<uint16_t>(i), op,
                                             static_cast<uint8_t>(i < r.opPred.n && r.opPred.d[i] == 1 ? 1 : 0) };
                        if (!r.preds.Push(pr)) {
                            return false;
                        }
                    } else {
                        T.cmp |= T.t[g];
                    }
                    const bool flushed = i < r.opFlush.n && r.opFlush.d[i] != 0;
                    const bool differs = i < r.opPred.n && r.opPred.d[i] == 2;
                    if (flushed) {
                        /* the drain (its prim depth / blend alpha: the drain's events) */
                        T.Observe(Bit(RG_PACK), Bit(RG_PACK) | Bit(RG_TRIMEMO));
                    } else if (differs) {
                        /* folded into the batch ([native] ownfold): the fold test reads the batch
                         * and the bound shader */
                        T.Observe(Bit(RG_PACK) | Bit(RG_RS0), Bit(RG_PACK));
                    }
                }
                T.Full(g);
                break;
            }
            case OP_SETFILLCOLOR: T.Full(RG_FILL); break;
            case OP_SETSCISSOR:
                T.Full(RG_SCIS);
                T.Full(RG_VSC);
                break;
            case OP_SETTIMG: T.Full(RG_TTL); break;
            case OP_SETTILE: {
                const int t = static_cast<int>(C1(w[i].w1, 24, 3));
                T.Partial(RG_TXC, Bit(RG_TILEF0 + t)); /* armed only when a field changes */
                T.Full(RG_TILEF0 + t);
                break;
            }
            case OP_SETTILESIZE: {
                const int t = static_cast<int>(C1(w[i].w1, 24, 3));
                T.Partial(RG_TXC, Bit(RG_TILES0 + t));
                T.Full(RG_TILES0 + t);
                break;
            }
            case OP_LOADBLOCK: {
                const int t = static_cast<int>(C1(w[i].w1, 24, 3));
                T.Observe(Bit(RG_TTL) | Bit(RG_TILEF0 + t), Bit(RG_TTL) | Bit(RG_TXC));
                break;
            }
            case OP_GEOMETRYMODE: T.Partial(RG_GEOM, 0); break;
            case OP_EXTRAGEOMETRYMODE: T.Partial(RG_XGEOM, 0); break;
            case OP_TEXTURE:
                T.Partial(RG_TXC, Bit(RG_FTI));
                T.Full(RG_FTI);
                T.Full(RG_TSCALE);
                break;
            case OP_SETKEYR:
            case OP_SETKEYGB: T.Partial(RG_KEY, 0); break;
            case OP_SETCONVERT: T.Full(RG_CONV); break;
            case OP_FILLRECT:
            case OP_TEXRECT:
            case OP_TEXRECTFLIP: {
                const bool fill = op == OP_FILLRECT;
                const uint8_t path = fill && i < r.opFill.n ? r.opFill.d[i] : 0;
                if (path == 1) {
                    T.Observe(Bit(RG_ZCI), 0); /* the branch that draws nothing */
                    break;
                }
                if (path == 2) {
                    /* the depth clear: a drain (FLUSH_FBOP) and ClearDepthRegion */
                    T.Observe(Bit(RG_ZCI) | Bit(RG_PACK), Bit(RG_PACK) | Bit(RG_TRIMEMO));
                    break;
                }
                const uint8_t known = i < r.opTex.n ? r.opTex.d[i] : 0;
                const uint8_t texMask = (known & 0x80u) != 0 ? static_cast<uint8_t>(known & 3u) : 3u;
                const bool vtxColour = (known & 0x80u) == 0 || (known & 0x40u) != 0;
                const bool blendRgb = (known & 0x80u) == 0 || (known & 0x20u) != 0;
                const uint8_t regs = (known & 0x80u) == 0 ? 0x0Fu : (i < r.opRegs.n ? r.opRegs.d[i] : 0x0Fu);
                const uint8_t tile = fill ? (i < r.opTile.n ? r.opTile.d[i] : 0)
                                          : static_cast<uint8_t>(C1(w[i].w1, 24, 3));
                T.Observe(DrawReads(tile, texMask, fill, vtxColour, blendRgb, regs) | Bit(RG_ZCI),
                          DrawWrites(texMask, fill));
                if (!fill) {
                    i += 2;
                }
                break;
            }
            default: return false;
        }
    }
    /* every group the run writes is applied from the record: its value must be determined */
    T.cmp |= T.Of(T.wr);
    *cmpOut = T.cmp;
    *writeOut = T.wr;
    return true;
}

/* The test DpSet{Prim,Env,Fog,Blend}Color / DpSetPrimDepth make against the live register before a
 * value change may split the open batch: true when the new value leaves it unchanged. */
bool Interp::HudColourSame(uint8_t op, uint32_t w0, uint32_t w1) const {
    const Rgba c = { static_cast<uint8_t>(w1 >> 24), static_cast<uint8_t>(w1 >> 16), static_cast<uint8_t>(w1 >> 8),
                     static_cast<uint8_t>(w1) };
    switch (op) {
        case OP_SETPRIMCOLOR:
            return SameRgba(mRdp.prim_color, c) && mRdp.prim_lod_fraction == static_cast<uint8_t>(C0(w0, 0, 8));
        case OP_SETENVCOLOR: return SameRgba(mRdp.env_color, c);
        case OP_SETFOGCOLOR: return SameRgba(mRdp.fog_color, c);
        case OP_SETBLENDCOLOR: return mRdp.blend_color.a == c.a;
        case OP_SETPRIMDEPTH: return mRdp.prim_depth == static_cast<uint16_t>((w1 >> 16) & 0x7FFFu);
        default: return false;
    }
}

/* ---- the run's entry ----------------------------------------------------------------------- */

namespace {

bool RetainableState(int trimemo, bool verifyActive, bool texOv, int tmemSkip, int tmemCls, int dspCensus) {
    return trimemo == 0 && !verifyActive && !texOv && tmemSkip == 0 && tmemCls == 0 && dspCensus == 0;
}

} // namespace

/* An immutable load's same-content test (SameContentLoad) sees the span at its base as the run
 * has left it so far. The plan replays the run's span stores on a word-owner map -- each TMEM
 * word is the ENTRY state's, retired (NONE), or a span this run stored -- with StoreLoadedTexture's
 * retire rule (a store retires every span it overlaps, whole). A test that sees one of the
 * run's own spans is decided by the recorded descriptors; one that sees an intact ENTRY span is
 * asked again, on the live entry state; a retired base fails. Every immutable load's answer
 * must be the recorded one. Non-immutable loads always store (SameContentLoad is false). */
bool Interp::HudRetainSameContentPlan(const HudRetRec& rec) const {
    bool anyImm = false;
    for (uint32_t k = 0; k < rec.loads.n && !anyImm; k++) {
        anyImm = rec.loads.d[k].imm != 0;
    }
    if (!anyImm) {
        return true;
    }
    /* the HUD's shape: every load at one TMEM base (gDPLoadTextureBlock to TMEM 0). The span at
     * that base is the entry's until the run's first store, then the last store's (a store
     * retires whatever overlapped it), so each test is decided without the word map. */
    const uint32_t base0 = rec.loads.d[0].tmemIndex;
    bool oneBase = base0 < 512u;
    for (uint32_t k = 1; k < rec.loads.n && oneBase; k++) {
        oneBase = rec.loads.d[k].tmemIndex == base0;
    }
    const auto wordsOfForm = [](const LoadedTexture& t, uint32_t base) {
        const uint32_t logical = t.orig_size_bytes != 0 ? t.orig_size_bytes : t.size_bytes;
        uint32_t words = (logical + 7u) / 8u;
        if (words < 1) {
            words = 1;
        }
        return words < 512u - base ? words : 512u - base;
    };
    if (oneBase) {
        int last = -1;
        for (uint32_t k = 0; k < rec.loads.n; k++) {
            const HudLoadForm& ld = rec.loads.d[k];
            if (ld.imm != 0) {
                bool same;
                if (last < 0) {
                    same = SameContentLoad(ld.tmemIndex, 1, ld.loaded);
                } else {
                    const HudLoadForm& j = rec.loads.d[last];
                    same = wordsOfForm(j.loaded, base0) == wordsOfForm(ld.loaded, base0) &&
                           j.loaded.addr == ld.loaded.addr && j.loaded.orig_size_bytes == ld.loaded.orig_size_bytes &&
                           j.loaded.size_bytes == ld.loaded.size_bytes &&
                           j.loaded.full_image_line_size_bytes == ld.loaded.full_image_line_size_bytes &&
                           j.loaded.line_size_bytes == ld.loaded.line_size_bytes &&
                           j.loaded.native_rgba16 == ld.loaded.native_rgba16 &&
                           j.loaded.immutable == ld.loaded.immutable;
                }
                if (same != (ld.skipped != 0)) {
                    return false;
                }
            }
            if (ld.skipped == 0) {
                last = static_cast<int>(k);
            }
        }
        return true;
    }
    constexpr int16_t kEntry = -1, kNone = -2;
    int16_t owner[512];
    for (uint32_t w = 0; w < 512u; w++) {
        owner[w] = kEntry;
    }
    const auto wordsOf = [](const LoadedTexture& t, uint32_t base) {
        const uint32_t logical = t.orig_size_bytes != 0 ? t.orig_size_bytes : t.size_bytes;
        uint32_t words = (logical + 7u) / 8u;
        if (words < 1) {
            words = 1;
        }
        return words < 512u - base ? words : 512u - base;
    };
    for (uint32_t k = 0; k < rec.loads.n; k++) {
        const HudLoadForm& ld = rec.loads.d[k];
        const uint32_t base = ld.tmemIndex;
        if (base >= 512u) {
            return false;
        }
        const uint32_t want = wordsOf(ld.loaded, base);
        if (ld.imm != 0) {
            bool same = false;
            const int16_t o = owner[base];
            if (o >= 0) {
                /* one of this run's spans: live at `base` only when it starts there */
                const HudLoadForm& j = rec.loads.d[o];
                same = j.tmemIndex == base && wordsOf(j.loaded, base) == want && j.loaded.addr == ld.loaded.addr &&
                       j.loaded.orig_size_bytes == ld.loaded.orig_size_bytes &&
                       j.loaded.size_bytes == ld.loaded.size_bytes &&
                       j.loaded.full_image_line_size_bytes == ld.loaded.full_image_line_size_bytes &&
                       j.loaded.line_size_bytes == ld.loaded.line_size_bytes &&
                       j.loaded.native_rgba16 == ld.loaded.native_rgba16 && j.loaded.immutable == ld.loaded.immutable;
            } else if (o == kEntry) {
                /* the entry span, if it is still whole (nothing of this run overlapped it) */
                bool whole = mRdp.tmem_span_base[base] == static_cast<uint16_t>(base + 1u);
                const uint32_t ew = whole ? mRdp.tmem_span_words[base] : 0u;
                for (uint32_t w = base; w < base + ew && w < 512u && whole; w++) {
                    whole = owner[w] == kEntry;
                }
                same = whole && SameContentLoad(ld.tmemIndex, 1, ld.loaded);
            }
            if (same != (ld.skipped != 0)) {
                return false;
            }
        }
        if (ld.skipped != 0) {
            continue;
        }
        /* the store: every span overlapping [base, base + want) is retired whole */
        const uint32_t end = base + want;
        for (uint32_t w = base; w < end; w++) {
            const int16_t o = owner[w];
            if (o >= 0) {
                const uint32_t jb = rec.loads.d[o].tmemIndex;
                const uint32_t je = jb + wordsOf(rec.loads.d[o].loaded, jb);
                for (uint32_t x = jb; x < je && x < 512u; x++) {
                    if (owner[x] == o) {
                        owner[x] = kNone;
                    }
                }
            } else if (o == kEntry && mRdp.tmem_span_base[w] != 0) {
                const uint32_t eb = mRdp.tmem_span_base[w] - 1u;
                const uint32_t ee = eb + mRdp.tmem_span_words[eb];
                for (uint32_t x = eb; x < ee && x < 512u; x++) {
                    if (owner[x] == kEntry) {
                        owner[x] = kNone;
                    }
                }
            }
        }
        for (uint32_t w = base; w < end; w++) {
            owner[w] = static_cast<int16_t>(k);
        }
    }
    return true;
}

/* Does the recorded run apply here, exactly? `why` gets the first failing fact. */
bool Interp::HudRetainFingerprint(const HudRetRec& rec, const void* runWords, uint32_t n, bool checkWords, int* why) {
    HudRetain& R = *mHudR;
    if (!rec.valid) {
        *why = RM_EMPTY;
        return false;
    }
    if (rec.nWords != n || (checkWords && memcmp(rec.words.d, runWords, sizeof(GdxHudWord) * n) != 0)) {
        *why = RM_WORDS;
        return false;
    }
    const bool ph = mHudRetClock >= 2;
    uint64_t p0 = ph ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
    const auto phase = [&](int k) {
        if (ph) {
            const uint64_t p1 = static_cast<uint64_t>(gdx_ninterp_now());
            R.wTicksFpPh[k] += p1 - p0;
            p0 = p1;
        }
    };
    for (uint64_t m = rec.cmpMask; m != 0; m &= m - 1u) {
        const int g = __builtin_ctzll(m);
        if (!HudRegSameLive(rec.entry, g)) {
            R.wRegMiss[g]++;
            R.lastGroup = static_cast<uint8_t>(g);
            *why = RM_REGS;
            return false;
        }
    }
    phase(0);
    if (rec.timgEpoch != mSrc.TimgClassEpoch() || rec.atlasEpoch != mBackend->AtlasEpoch() ||
        rec.texRoom != (mTexCount + 1 < kTexCacheSize) || memcmp(&rec.cfg, &mCfg, sizeof(mCfg)) != 0) {
        *why = RM_EXT;
        return false;
    }
    for (int u = 0; u < 2; u++) {
        const int16_t s = mRs.textures[u];
        const uint32_t id = s >= 0 ? mTex[s].texture_id : 0xFFFFFFFFu;
        if (((rec.cmpMask >> (u == 0 ? RG_RS0 : RG_RS1)) & 1u) != 0 && id != rec.entryTexId[u]) {
            *why = RM_TEX;
            return false;
        }
    }
    for (uint32_t k = 0; k < rec.imps.n; k++) {
        const RetImp& im = rec.imps.d[k];
        const TexEntry& e = mTex[im.slot];
        if (!e.used || !e.uploaded || e.texture_id != im.texId || memcmp(&e.key, &im.key, sizeof(TexKey)) != 0) {
            *why = RM_TEX;
            return false;
        }
    }
    for (uint32_t k = 0; k < rec.foreign.n; k++) {
        const RetForeign& fo = rec.foreign.d[k];
        const LoadedTexture& lt = mRdp.loaded_texture[fo.base];
        const bool span = mRdp.tmem_span_base[fo.word] == static_cast<uint16_t>(fo.base + 1u) &&
                          mRdp.tmem_span_words[fo.base] == fo.spanWords && mRdp.tmem_span_load_kind[fo.base] == fo.kind &&
                          lt.addr == fo.lt.addr && lt.orig_size_bytes == fo.lt.orig_size_bytes &&
                          lt.size_bytes == fo.lt.size_bytes &&
                          lt.full_image_line_size_bytes == fo.lt.full_image_line_size_bytes &&
                          lt.line_size_bytes == fo.lt.line_size_bytes && lt.tmem_start == fo.lt.tmem_start &&
                          lt.tmem_word_count == fo.lt.tmem_word_count && lt.immutable == fo.lt.immutable &&
                          lt.native_rgba16 == fo.lt.native_rgba16;
        if (!span || (fo.src != nullptr && !HudDebtCoverSame(fo.off, fo.span, fo.src, fo.native, fo.cls))) {
            *why = RM_TEX;
            return false;
        }
    }
    phase(1);
    uint64_t now[2];
    gdx_gfx_tmem_source_stamp(now);
    for (uint32_t k = 0; k < rec.foreign.n; k++) {
        const RetForeign& fo = rec.foreign.d[k];
        if (fo.src != nullptr &&
            gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(fo.src), fo.span, fo.cls, rec.srcStamp) == 0) {
            *why = RM_SRC;
            return false;
        }
    }
    if (!HudRetainSameContentPlan(rec)) {
        *why = RM_TEX;
        return false;
    }
    for (uint32_t k = 0; k < rec.preds.n; k++) {
        const RetPred& pr = rec.preds.d[k];
        const GdxHudWord& w = rec.words.d[pr.word];
        if (HudColourSame(pr.op, w.w0, w.w1) != (pr.equal != 0)) {
            R.wRegMiss[RG_PRIM]++;
            R.lastGroup = 0xFF; /* a predicate */
            *why = RM_REGS;
            return false;
        }
    }
    phase(2);
    for (uint32_t k = 0; k < rec.srcs.n; k++) {
        const RetSrc& sr = rec.srcs.d[k];
        if (gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(sr.addr), sr.n, sr.cls, rec.srcStamp) == 0) {
            *why = RM_SRC;
            return false;
        }
    }
    phase(3);
    const size_t needFloats = (static_cast<size_t>(rec.tris) + static_cast<size_t>(kMaxTriBuffer)) * kPackTriFloats;
    if (mBackend->VboFreeFloats() < needFloats) {
        *why = RM_HEAD;
        return false;
    }
    /* Unchanged through `now`: the next check scans from here (sampled before the checks). */
    HudRetRec& mut = const_cast<HudRetRec&>(rec);
    mut.srcStamp[0] = now[0];
    mut.srcStamp[1] = now[1];
    *why = RM_NONE;
    return true;
}


/* ---- the variant hit: only rectangle geometry moved ---------------------------------------- */

namespace {

bool SameWord(const GdxHudWord& a, const GdxHudWord& b) {
    return a.w0 == b.w0 && a.w1 == b.w1 && a.w1hi == b.w1hi;
}

/* == DpSetFillColor */
Rgba FillColorOf(uint32_t packed) {
    const uint16_t col16 = static_cast<uint16_t>(packed);
    Rgba c;
    c.r = static_cast<uint8_t>(((col16 >> 11) & 0x1F) * 255 / 31);
    c.g = static_cast<uint8_t>(((col16 >> 6) & 0x1F) * 255 / 31);
    c.b = static_cast<uint8_t>(((col16 >> 1) & 0x1F) * 255 / 31);
    c.a = static_cast<uint8_t>((col16 & 1) * 255);
    return c;
}

/* May word i of a run differ between its record and this frame for a variant hit? Only the
 * geometry operands of a rectangle (its corners, texture coordinates and steps -- never its
 * tile or opcode) and a fill colour's value. */
bool GeometryOnlyDiff(const GdxHudWord* nw, const GdxHudWord* ow, uint32_t n, bool* any) {
    *any = false;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t op = Opcode(ow[i].w0);
        const bool rect3 = op == OP_TEXRECT || op == OP_TEXRECTFLIP;
        const uint32_t span = rect3 ? 3u : 1u;
        bool same = true;
        for (uint32_t k = 0; k < span && i + k < n; k++) {
            same = same && SameWord(nw[i + k], ow[i + k]);
        }
        if (!same) {
            if (Opcode(nw[i].w0) != op || nw[i].w1hi != ow[i].w1hi) {
                return false;
            }
            if (rect3) {
                if (i + 2 >= n || C1(nw[i].w1, 24, 3) != C1(ow[i].w1, 24, 3) || nw[i + 1].w0 != ow[i + 1].w0 ||
                    nw[i + 2].w0 != ow[i + 2].w0 || nw[i + 1].w1hi != ow[i + 1].w1hi ||
                    nw[i + 2].w1hi != ow[i + 2].w1hi) {
                    return false;
                }
            } else if (op == OP_SETFILLCOLOR) {
                if (nw[i].w0 != ow[i].w0) {
                    return false;
                }
            } else if (op != OP_FILLRECT) {
                return false;
            }
            *any = true;
        }
        i += span - 1u;
    }
    return true;
}

/* DpFillRectangle's corners for a normal (drawn, not full-screen) fill rectangle, or false. */
bool FillCorners(const GdxHudWord& w, uint32_t omH, uint32_t nativeW, uint32_t nativeH, int32_t out[4]) {
    int32_t ulx = static_cast<int32_t>(C1(w.w1, 12, 12));
    int32_t uly = static_cast<int32_t>(C1(w.w1, 0, 12));
    int32_t lrx = static_cast<int32_t>(C0(w.w0, 12, 12));
    int32_t lry = static_cast<int32_t>(C0(w.w0, 0, 12));
    if (ulx > lrx || uly > lry) {
        return false;
    }
    if (ulx == 0 && uly == 0 && lrx == static_cast<int32_t>(nativeW - 1) * 4 &&
        lry == static_cast<int32_t>(nativeH - 1) * 4) {
        return false; /* the full-screen widening: not a variant */
    }
    const uint32_t mode = omH & (3u << MDSFT_CYCLETYPE);
    if (mode == CYC_COPY || mode == CYC_FILL) {
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    out[0] = ulx;
    out[1] = uly;
    out[2] = lrx;
    out[3] = lry;
    return true;
}

/* The float offset of triangle `tri` of batch `serial` in the record, or -1. */
int32_t TriFloatOffset(const Interp::HudRetRec& rec, uint32_t serial, uint32_t tri);

} // namespace

/* A rectangle's four corners packed exactly as its emission packed them: the recorded batch
 * parameters, tiles, colour registers and combiner state installed for the call. */
void Interp::HudRetainPackRect(const void* rectCtx, const LoadedVertex quad[4], float* corners, Gdx3dsVboPackAux* aux) {
    const RetRect& rc = *static_cast<const RetRect*>(rectCtx);
    const Gdx3dsVboPackParams params = mPackParams;
    const uint32_t e0 = rc.ds.effective_tile[0] & 7u;
    const uint32_t e1 = rc.ds.effective_tile[1] & 7u;
    const Tile t0 = mRdp.texture_tile[e0];
    const Tile t1 = mRdp.texture_tile[e1];
    const Rgba prim = mRdp.prim_color, env = mRdp.env_color, fog = mRdp.fog_color, blend = mRdp.blend_color;
    const Rgba keyC = mRdp.key_center, keyS = mRdp.key_scale;
    const uint8_t primLod = mRdp.prim_lod_fraction;
    const uint32_t omL = mRdp.other_mode_l;
    int16_t conv[6];
    memcpy(conv, mRdp.convert_k, sizeof(conv));
    mPackParams = rc.params;
    mRdp.texture_tile[e1] = rc.tiles[1];
    mRdp.texture_tile[e0] = rc.tiles[0];
    mRdp.prim_color = rc.prim;
    mRdp.env_color = rc.env;
    mRdp.fog_color = rc.fog;
    mRdp.blend_color = rc.blend;
    mRdp.key_center = rc.keyC;
    mRdp.key_scale = rc.keyS;
    mRdp.prim_lod_fraction = rc.primLod;
    mRdp.other_mode_l = rc.omL;
    memcpy(mRdp.convert_k, rc.conv, sizeof(conv));
    const LoadedVertex* const qp[4] = { &quad[0], &quad[1], &quad[2], &quad[3] };
    HudPackVerts(rc.ds, qp, 4, &quad[0], corners);
    if (aux != nullptr) {
        memset(aux, 0, sizeof(*aux));
        HudPackAux(rc.ds, &quad[0], aux);
    }
    memcpy(mRdp.convert_k, conv, sizeof(conv));
    mRdp.other_mode_l = omL;
    mRdp.prim_lod_fraction = primLod;
    mRdp.key_scale = keyS;
    mRdp.key_center = keyC;
    mRdp.blend_color = blend;
    mRdp.fog_color = fog;
    mRdp.env_color = env;
    mRdp.prim_color = prim;
    mRdp.texture_tile[e0] = t0;
    mRdp.texture_tile[e1] = t1;
    mPackParams = params;
}

namespace {

int32_t TriFloatOffset(const Interp::HudRetRec& rec, uint32_t serial, uint32_t tri) {
    for (uint32_t k = 0; k < rec.evs.n; k++) {
        const RetEv& e = rec.evs.d[k];
        if (e.op == EV_FLOATS && static_cast<uint32_t>(e.i[3]) == serial && tri >= static_cast<uint32_t>(e.i[0]) &&
            tri < static_cast<uint32_t>(e.i[0] + e.i[1])) {
            return e.i[2] + static_cast<int32_t>((tri - static_cast<uint32_t>(e.i[0])) * kPackTriFloats);
        }
    }
    return -1;
}

/* The aux record the backend will draw batch `serial` with: its DRAW's, or the exit pack
 * state's when the batch is still open at the run's end. */
Gdx3dsVboPackAux* BatchAux(Interp::HudRetRec& rec, uint32_t serial, uint32_t lastSerial) {
    for (uint32_t k = 0; k < rec.evs.n; k++) {
        const RetEv& e = rec.evs.d[k];
        if (e.op == EV_DRAW && static_cast<uint32_t>(e.i[2]) == serial) {
            return &rec.auxs.d[e.i[1]];
        }
    }
    return serial == lastSerial && rec.exit.bufNumTris > 0 ? &rec.exit.aux : nullptr;
}

} // namespace

/* This frame's run differs from its record only in rectangle geometry: recompute every
 * rectangle's corners from this frame's words and the recorded state, pack them with the
 * recorded parameters, check that every rectangle whose inputs did not change packs to exactly
 * the recorded floats (the self-check), then write the moved ones into the record. The record
 * then IS this frame's run, and replays. */
bool Interp::HudRetainVariant(HudRetRec& rec, const void* runWords, uint32_t n) {
    HudRetain& R = *mHudR;
    const GdxHudWord* nw = static_cast<const GdxHudWord*>(runWords);
    bool any = false;
    if (!rec.valid || rec.nWords != n || rec.rectCtx.n == 0 || !GeometryOnlyDiff(nw, rec.words.d, n, &any) || !any) {
        return false;
    }
    int why = RM_NONE;
    if (!HudRetainFingerprint(rec, runWords, n, false, &why)) {
        R.wVarWhy[0]++;
        R.wVarNo++;
        return false;
    }
    const uint32_t nr = rec.rectCtx.n;
    if (!R.varFloats.Reserve(nr * 2u * static_cast<uint32_t>(kPackTriFloats)) || !R.varAux.Reserve(nr) ||
        !R.varDirty.Reserve(nr)) {
        return false;
    }
    uint32_t lastSerial = 0;
    for (uint32_t k = 0; k < rec.evs.n; k++) {
        lastSerial += rec.evs.d[k].op == EV_BEGIN ? 1u : 0u;
    }
    LoadedVertex cur[4];
    memcpy(cur, &mRsp.loaded_vertices[kMaxVertices], sizeof(cur));
    Rgba fill = mRdp.fill_color;
    uint32_t ri = 0;
    const uint32_t omHLive = mRdp.other_mode_h;
    const uint32_t xgeomLive = mRsp.extra_geometry_mode;
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) {
        const uint8_t op = Opcode(nw[i].w0);
        if (op == OP_SETFILLCOLOR) {
            fill = FillColorOf(nw[i].w1);
            continue;
        }
        if (op != OP_TEXRECT && op != OP_TEXRECTFLIP && op != OP_FILLRECT) {
            continue;
        }
        const bool fillOp = op == OP_FILLRECT;
        const RetRect* rc = ri < nr ? &rec.rectCtx.d[ri] : nullptr;
        if (rc == nullptr || rc->word != i || rc->op != op) {
            R.wVarWhy[1]++;
            ok = false;
            break;
        }
        const bool wordsMoved = !SameWord(nw[i], rec.words.d[i]) ||
                                (!fillOp && (!SameWord(nw[i + 1], rec.words.d[i + 1]) || !SameWord(nw[i + 2], rec.words.d[i + 2])));
        /* the inputs a rectangle reads from the corner slots and the fill colour */
        bool inputsIn = false;
        for (int k = 0; k < 4 && !inputsIn; k++) {
            inputsIn = cur[k].clip_rej != rc->lv[k].clip_rej || cur[k].clip_out != rc->lv[k].clip_out ||
                       (fillOp ? !SameBytes(&cur[k].u, &rc->lv[k].u, 2 * sizeof(float))
                               : !SameRgba(cur[k].color, rc->lv[k].color));
        }
        inputsIn = inputsIn || (fillOp && !SameRgba(fill, rc->lv[0].color));
        if (!wordsMoved && !inputsIn && mHudRetain != 2) {
            /* unmoved: its floats and what it leaves in the corner slots are the recorded ones
             * (hud_retain=2 recomputes and self-checks them instead) */
            R.varDirty.d[ri] = 0;
            for (int k = 0; k < 4; k++) {
                cur[k].x = rc->lv[k].x;
                cur[k].y = rc->lv[k].y;
                cur[k].z = rc->lv[k].z;
                cur[k].w = rc->lv[k].w;
                if (fillOp) {
                    cur[k].color = rc->lv[k].color;
                } else {
                    cur[k].u = rc->lv[k].u;
                    cur[k].v = rc->lv[k].v;
                }
            }
            i += fillOp ? 0u : 2u;
            ri++;
            continue;
        }
        LoadedVertex quad[4];
        memcpy(quad, cur, sizeof(quad));
        bool inputsMoved = false;
        if (fillOp) {
            int32_t cr[4];
            mRdp.other_mode_h = rc->omH;
            mRsp.extra_geometry_mode = rc->xgeom;
            const bool normal = FillCorners(nw[i], rc->omH, mCfg.nativeW, mCfg.nativeH, cr);
            if (normal) {
                HudRectPositions(cr[0], cr[1], cr[2], cr[3], quad);
            }
            mRdp.other_mode_h = omHLive;
            mRsp.extra_geometry_mode = xgeomLive;
            if (!normal || mRdp.color_image_address == mRdp.z_buf_address) {
                R.wVarWhy[2]++;
                ok = false;
                break;
            }
            for (int k = 0; k < 4; k++) {
                quad[k].color = fill;
            }
            inputsMoved = !SameRgba(fill, rc->lv[0].color);
            for (int k = 0; k < 4 && !inputsMoved; k++) {
                inputsMoved = !SameBytes(&cur[k].u, &rc->lv[k].u, 2 * sizeof(float));
            }
        } else {
            const Cmd c = { nw[i].w0, nw[i].w1, nw[i].w1hi };
            const Cmd h1 = { nw[i + 1].w0, nw[i + 1].w1, nw[i + 1].w1hi };
            const Cmd h2 = { nw[i + 2].w0, nw[i + 2].w1, nw[i + 2].w1hi };
            const uint8_t tile = static_cast<uint8_t>(C1(c.w1, 24, 3));
            float lrs = 0.0f, lrt = 0.0f;
            mRdp.other_mode_h = rc->omH;
            mRsp.extra_geometry_mode = rc->xgeom;
            (void)HudRectGeometry(c, h1, h2, quad, &lrs, &lrt);
            const Tile saved = mRdp.texture_tile[tile];
            mRdp.texture_tile[tile] = rc->tiles[0];
            const bool elig = mTrectBatchOn &&
                              HudTrectEligible(tile, static_cast<int16_t>(C1(h1.w1, 16, 16)),
                                               static_cast<int16_t>(C1(h1.w1, 0, 16)), lrs, lrt);
            mRdp.texture_tile[tile] = saved;
            mRdp.other_mode_h = omHLive;
            mRsp.extra_geometry_mode = xgeomLive;
            if ((elig ? 1 : 0) != rc->trectOk || (rc->ds.effective_tile[0] & 7u) != tile) {
                R.wVarWhy[3]++;
                ok = false;
                break;
            }
            for (int k = 0; k < 4 && !inputsMoved; k++) {
                inputsMoved = !SameRgba(cur[k].color, rc->lv[k].color);
            }
        }
        for (int k = 0; k < 4 && !inputsMoved; k++) {
            inputsMoved = cur[k].clip_rej != rc->lv[k].clip_rej || cur[k].clip_out != rc->lv[k].clip_out;
        }
        const bool moved = wordsMoved || inputsMoved;
        if (!rc->ok && moved) {
            R.wVarWhy[4]++;
            ok = false;
            break;
        }
        R.varDirty.d[ri] = moved ? 1 : 0;
        if (rc->ok) {
            float corners[4 * GDX3DS_VBOPACK_STRIDE];
            Gdx3dsVboPackAux aux;
            HudRetainPackRect(rc, quad, corners, rc->began ? &aux : nullptr);
            static const int kOrder[6] = { 0, 1, 3, 1, 2, 3 }; /* SpTri(ul, ll, ur), SpTri(ll, lr, ur) */
            float* out = R.varFloats.d + ri * 2u * kPackTriFloats;
            for (int k = 0; k < 6; k++) {
                memcpy(out + k * GDX3DS_VBOPACK_STRIDE, corners + kOrder[k] * GDX3DS_VBOPACK_STRIDE,
                       GDX3DS_VBOPACK_STRIDE * sizeof(float));
            }
            R.varAux.d[ri] = aux;
            const int32_t off0 = TriFloatOffset(rec, rc->serial, rc->tri0);
            const int32_t off1 = TriFloatOffset(rec, rc->serial, rc->tri0 + 1u);
            if (off0 < 0 || off1 != off0 + static_cast<int32_t>(kPackTriFloats)) {
                ok = !moved;
                R.wVarWhy[5] += moved ? 1u : 0u;
                R.varDirty.d[ri] = 0;
            } else if (!moved) {
                /* the self-check: an unmoved rectangle packs to exactly its recorded floats */
                const Gdx3dsVboPackAux* ra = rc->began ? BatchAux(rec, rc->serial, lastSerial) : nullptr;
                if (memcmp(out, rec.floats.d + off0, 2u * kPackTriFloats * sizeof(float)) != 0 ||
                    (ra != nullptr && !SameAux(aux, *ra))) {
                    R.wVarSelfBad++;
                    R.wVarWhy[6]++;
                    ok = false;
                }
            }
        }
        /* what the rectangle leaves in the four corner slots */
        for (int k = 0; k < 4; k++) {
            cur[k].x = quad[k].x;
            cur[k].y = quad[k].y;
            cur[k].z = quad[k].z;
            cur[k].w = quad[k].w;
            if (fillOp) {
                cur[k].color = quad[k].color;
            } else {
                cur[k].u = quad[k].u;
                cur[k].v = quad[k].v;
            }
        }
        i += fillOp ? 0u : 2u;
        ri++;
    }
    if (!ok || ri != nr) {
        R.wVarNo++;
        return false;
    }
    /* commit: the moved rectangles' floats and aux records, the exit corners and fill colour,
     * and this frame's words */
    uint32_t movedRects = 0;
    for (uint32_t k = 0; k < nr; k++) {
        const RetRect& rc = rec.rectCtx.d[k];
        if (R.varDirty.d[k] == 0) {
            continue;
        }
        movedRects++;
        const int32_t off0 = TriFloatOffset(rec, rc.serial, rc.tri0);
        memcpy(rec.floats.d + off0, R.varFloats.d + k * 2u * kPackTriFloats, 2u * kPackTriFloats * sizeof(float));
        if (rc.began) {
            Gdx3dsVboPackAux* ra = BatchAux(rec, rc.serial, lastSerial);
            if (ra != nullptr) {
                *ra = R.varAux.d[k];
            }
        }
    }
    memcpy(rec.exit.lv, cur, sizeof(cur));
    rec.exit.fill = fill;
    memcpy(rec.words.d, runWords, sizeof(GdxHudWord) * n);
    R.wVarHit++;
    R.wVarRects += movedRects;
    return true;
}

/* A hit: the recorded effects, in the recorded order. */
void Interp::HudRetainReplay(HudRetRec& rec) {
    HudRetain& R = *mHudR;
    const bool clk = mHudRetClock != 0;
    uint64_t c0 = clk ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
    for (uint32_t k = 0; k < rec.loads.n; k++) {
        HudDirectApplyLoad(rec.loads.d[k], false);
    }
    for (uint32_t k = 0; k < rec.imps.n; k++) {
        TexCacheTouch(rec.imps.d[k].slot);
    }
    if (GDX_UNLIKELY(clk)) {
        const uint64_t c1 = static_cast<uint64_t>(gdx_ninterp_now());
        R.wTicksRLoad += c1 - c0;
        c0 = c1;
    }
    float* cursor = mPackDst;
    int armLast = -1;
    bool begunHere = false; /* C6: the open batch was begun by this replay, not before the run */
    for (uint32_t k = 0; k < rec.evs.n; k++) {
        const RetEv& e = rec.evs.d[k];
        switch (e.op) {
            case EV_UNLOAD: mBackend->UnloadShader(const_cast<void*>(e.p)); break;
            case EV_LOAD: mBackend->LoadShader(const_cast<void*>(e.p)); break;
            case EV_SELTEX: mBackend->SelectTexture(e.i[0], static_cast<uint32_t>(e.i[1])); break;
            case EV_SAMPLER:
                mBackend->SetSamplerParameters(e.i[0], e.a != 0, static_cast<uint32_t>(e.i[1]),
                                               static_cast<uint32_t>(e.i[2]));
                break;
            case EV_DEPTH: mBackend->SetDepthTestAndMask(e.a != 0, e.b != 0); break;
            case EV_DECAL: mBackend->SetZmodeDecal(e.a != 0); break;
            case EV_VIEWPORT: mBackend->SetViewport(e.i[0], e.i[1], e.i[2], e.i[3]); break;
            case EV_SCISSOR: mBackend->SetScissor(e.i[0], e.i[1], e.i[2], e.i[3]); break;
            case EV_ALPHA: mBackend->SetUseAlpha(e.a != 0); break;
            case EV_PRIMDEPTH:
                /* == Flush's expression; an entry value (e.c) is the live register's */
                mBackend->SetCurrentPrimDepth(e.c != 0 ? static_cast<float>(mRdp.prim_depth) / 32767.0f : e.f);
                break;
            case EV_ALPHATHR:
                mBackend->SetCurrentAlphaCompareThreshold(
                    e.c != 0 ? static_cast<float>(mRdp.blend_color.a) / 255.0f : e.f);
                break;
            case EV_FOGNOTE: mBackend->FogNoteParams(e.i[0], e.i[1], e.i[2]); break;
            case EV_ATLASARM:
                /* the arm is read only by UploadTexture, which a retained run never reaches:
                   only the value the run leaves behind is applied (below) */
                armLast = static_cast<int>(e.a);
                break;
            case EV_CLEARDEPTH: mBackend->ClearDepthRegion(e.i[0], e.i[1], e.i[2], e.i[3]); break;
            case EV_REFRESH:
                /* VboPackRefresh only computes the open batch's params from the bound textures
                   (no backend state); the params it produced ride the record's exit pack state */
                break;
            case EV_BEGIN: {
                Gdx3dsVboPackParams scratch;
                uint32_t cap = 0;
                cursor = mBackend->VboPackBegin(&scratch, &cap);
                mStats.triBegin++;
                begunHere = true;
                break;
            }
            case EV_FLOATS:
                if (cursor != nullptr) {
                    memcpy(cursor + static_cast<size_t>(e.i[0]) * kPackTriFloats, rec.floats.d + e.i[2],
                           static_cast<size_t>(e.i[1]) * kPackTriFloats * sizeof(float));
                }
                break;
            case EV_DRAW:
                if (gdx3ds_replay_interp != nullptr) {
                    /* a batch open before the run holds THIS frame's triangles: the live tag */
                    gdx3ds_replay_interp->setDrawTag(RetainedReplayTag(begunHere, mReplayBatchTag));
                }
                mBackend->DrawPackedBatch(static_cast<size_t>(e.i[0]), &rec.auxs.d[e.i[1]]);
                mStats.batches++;
                cursor = nullptr;
                begunHere = false;
                break;
            default: break;
        }
    }
    if (armLast >= 0) {
        mBackend->AtlasArm(armLast != 0);
    }
    if (GDX_UNLIKELY(clk)) {
        const uint64_t c1 = static_cast<uint64_t>(gdx_ninterp_now());
        R.wTicksREv += c1 - c0;
        c0 = c1;
    }
    for (uint64_t m = rec.writeMask; m != 0; m &= m - 1u) {
        HudRegApply(rec.exit, __builtin_ctzll(m));
    }
    /* The pack state: the scalars always, the params/aux when the run wrote them, the pool
     * cursor from the replay (the batch open at exit was begun here or was already open). */
    if (((rec.writeMask >> RG_PACK) & 1u) != 0) {
        mBufNumTris = rec.exit.bufNumTris;
        mPackBatch = rec.exit.packBatch;
        mPackCapTris = rec.exit.packCap;
        mPackComb = rec.exit.packComb;
        mPackFolded = rec.exit.packFolded;
        if (rec.paramsWritten) {
            mPackParams = rec.exit.params;
        }
        if (rec.auxWritten) {
            mPackAux = rec.exit.aux;
        }
        mPackDst = mBufNumTris > 0 ? cursor : nullptr;
        if (begunHere) {
            mReplayBatchTag = GDX_REPLAY_TAG_SCREEN; /* C6: the batch the run left open is its own */
        }
    }
    if (GDX_UNLIKELY(clk)) {
        R.wTicksRApply += static_cast<uint64_t>(gdx_ninterp_now()) - c0;
    }
    R.wHit++;
    R.wHitRects += rec.rects;
    R.wHitLoads += rec.loads.n;
    R.wHitImps += rec.imps.n;
    R.wHitDraws += rec.draws;
    R.wHitTris += rec.tris;
}

/* A HUD run begins (direct mode). 1 = a hit was replayed and the run is done; 0 = execute it
 * (recording when retention is on). */
int Interp::HudRetainBegin(const void* runWords, uint32_t n) {
    if (mHudRetain == 0 || !HudRetainEnsure() || mHudR->recBackend == nullptr) {
        return 0;
    }
    InterpHwsScope hws(GDX3DS_HWS_HUDRET);
    HsxMark(0); /* [native] hudsky_emit, hudclock=2 */
    HudRetain& R = *mHudR;
    const uint32_t slot = R.runInTask; /* HudElemEnd / HudRetainEnd advance it */
    R.wRuns++;
    R.cur = nullptr;
    R.verifying = false;
    if (slot >= kRunSlots) {
        return 0;
    }
    if (n > kMaxRetainWords) {
        R.wUnret[RU_BIG]++;
        R.lastPath = 0;
        R.lastWhy = RM_EMPTY;
        return 0;
    }
    const uint32_t shape = ShapeKey(static_cast<const GdxHudWord*>(runWords), n);
    const uint32_t set = (shape ^ (shape >> 16)) & (kRunSlots - 1u);
    if (R.taskSeq < R.coolUntil[set]) {
        R.wCool++; /* a run that changed on each of its last recordings: executed, not recorded */
        R.lastPath = 0;
        R.lastWhy = RM_WORDS;
        return 0;
    }
    /* the way of this shape whose words match, else one of this shape (a variant candidate),
     * else the empty or least recently used one */
    HudRetRec** ways = R.rec[set];
    for (int k = 0; k < 2; k++) {
        if (ways[k] == nullptr) {
            ways[k] = RecNew();
            if (ways[k] == nullptr) {
                R.wUnret[RU_ALLOC]++;
                return 0;
            }
        }
    }
    int way = -1;
    bool exact = false;
    for (int k = 0; k < 2; k++) {
        const HudRetRec& w = *ways[k];
        if (w.structUnret && w.shape == shape && w.nWords == n &&
            memcmp(w.words.d, runWords, sizeof(GdxHudWord) * n) == 0) {
            R.wUnret[RU_OP]++; /* a shape already known to hold an op retention does not model */
            R.lastPath = 0;
            R.lastWhy = RM_EMPTY;
            return 0;
        }
    }
    for (int k = 0; k < 2; k++) {
        if (!ways[k]->valid || ways[k]->shape != shape || ways[k]->nWords != n) {
            continue;
        }
        if (memcmp(ways[k]->words.d, runWords, sizeof(GdxHudWord) * n) == 0) {
            way = k;
            exact = true;
            break;
        }
        if (way < 0) {
            way = k;
        }
    }
    if (way < 0) {
        way = !ways[0]->valid ? 0 : (!ways[1]->valid ? 1 : (R.mru[set] ^ 1));
    }
    R.mru[set] = static_cast<uint8_t>(way);
    HudRetRec& rec = *ways[way];
    int why = RM_EMPTY;
    const bool clk2 = mHudRetClock != 0;
    uint64_t c0 = clk2 ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
    const bool state = RetainableState(mTriMemoOn, mVerifyActive, mTexOv != nullptr, mTmemSkip, mTmemCls, mDspCensus);
    /* [native] hudsky_emit (gdx_hudsky_emit.inc): an exact-words hit is decided by the compiled
     * fingerprint (level 2: both, compared); -1 = the plan does not cover this record. */
    bool hit = false;
    const int hsxFp = (state && exact && mHudSkyEmit != 0 && mHudRetain == 1)
                          ? HsxExactFingerprint(rec, runWords, n, &why, &hit)
                          : -1;
    if (hsxFp < 0) {
        hit = state && HudRetainFingerprint(rec, runWords, n, !exact, &why);
    }
    R.lastPath = hit ? 1 : 0;
    if (GDX_UNLIKELY(clk2)) {
        const uint64_t c1 = static_cast<uint64_t>(gdx_ninterp_now());
        R.wTicksFp += c1 - c0;
        c0 = c1;
    }
    if (!hit && state && why == RM_WORDS && HudRetainVariant(rec, runWords, n)) {
        hit = true; /* only rectangle geometry moved: the record now holds this frame's floats */
        R.lastPath = 2;
    }
    if (GDX_UNLIKELY(clk2)) {
        const uint64_t c1 = static_cast<uint64_t>(gdx_ninterp_now());
        R.wTicksVar += c1 - c0;
        c0 = c1;
    }
    R.lastWhy = static_cast<uint8_t>(why);
    if (hit && mHudElem != 0 && (R.taskSeq % kElemEvery) == 0) {
        [&R, &rec, slot]() GDX_COLD {
        static const char kOpCh[EV_COUNT + 1] = "ulTsdcvxapafAREFDZ";
        char ops[160];
        uint32_t k = 0;
        for (; k < rec.evs.n && k + 1 < sizeof(ops); k++) {
            ops[k] = kOpCh[rec.evs.d[k].op < EV_COUNT ? rec.evs.d[k].op : EV_COUNT];
        }
        ops[k] = '\0';
        GDXN_LOG("[hudelem] task=%u run=%u evs=%u loads=%u imps=%u cmp=%016llx wr=%016llx ops=%s", R.taskSeq, slot,
                 rec.evs.n, rec.loads.n, rec.imps.n, static_cast<unsigned long long>(rec.cmpMask),
                 static_cast<unsigned long long>(rec.writeMask), ops);
        }();
    }
    if (hit) {
        R.missStreak[set] = 0;
    }
    if (hit && mHudRetain == 1) {
        const uint64_t t0 = (mHudClock != 0 || clk2) ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
        if (mHudSkyEmit == 1 && HsxPlanReady(rec)) {
            HsxReplay(rec); /* [native] hudsky_emit: the compiled plan, one backend program */
        } else {
            HudRetainReplay(rec);
            if (mHudSkyEmit == 2 && hsxFp == 2) {
                HsxVerifyApply(rec); /* the compiled apply on top of the stock one must change nothing */
            }
        }
        if (GDX_UNLIKELY(mHudClock != 0 || clk2)) {
            const uint64_t t1 = static_cast<uint64_t>(gdx_ninterp_now());
            R.wTicksHit += t1 - t0;
            if (GDX_UNLIKELY(clk2)) {
                R.wTicksReplay += t1 - c0;
            }
        }
        return 1;
    }
    if (!hit) {
        R.wMiss[why]++;
    }
    if (!state) {
        R.wUnret[RU_STATE]++;
        rec.valid = false;
        return 0;
    }
    if (!hit && ++R.missStreak[set] >= kMissStreak) {
        /* it changed on each of its last recordings: stop paying for records for a while */
        R.missStreak[set] = 0;
        R.coolUntil[set] = R.taskSeq + kCoolTasks;
    }
    /* A run whose record differed only in length-preserving words may move its geometry every
     * frame: record its rectangles' contexts so the next frame can be a variant hit. */
    R.wantCtx = hit || rec.rectCtx.n != 0 || (rec.valid && rec.nWords == n);
    /* Record this execution (and, on a would-be hit under verify, compare it afterwards). */
    HudRetRec& f = *R.fresh;
    RecClear(f);
    if (!f.words.Reserve(n)) {
        R.wUnret[RU_ALLOC]++;
        return 0;
    }
    memcpy(f.words.d, runWords, sizeof(GdxHudWord) * n);
    f.words.n = n;
    f.nWords = n;
    if (!f.opOpen.Reserve(n) || !f.opTex.Reserve(n) || !f.opTile.Reserve(n) || !f.opFill.Reserve(n) ||
        !f.opPred.Reserve(n) || !f.opFlush.Reserve(n) || !f.opRegs.Reserve(n)) {
        R.wUnret[RU_ALLOC]++;
        return 0;
    }
    memset(f.opOpen.d, 0, n);
    memset(f.opTex.d, 0, n);
    memset(f.opTile.d, 0, n);
    memset(f.opFill.d, 0, n);
    memset(f.opPred.d, 0, n);
    memset(f.opRegs.d, 0x0F, n);
    memset(f.opFlush.d, 0, n);
    f.opOpen.n = f.opTex.n = f.opTile.n = f.opFill.n = f.opPred.n = f.opFlush.n = f.opRegs.n = n;
    HudRegCapture(f.entry);
    f.timgEpoch = mSrc.TimgClassEpoch();
    f.atlasEpoch = mBackend->AtlasEpoch();
    f.texRoom = mTexCount + 1 < kTexCacheSize;
    f.cfg = mCfg;
    for (int u = 0; u < 2; u++) {
        const int16_t s = mRs.textures[u];
        f.entryTexId[u] = s >= 0 ? mTex[s].texture_id : 0xFFFFFFFFu;
    }
    gdx_gfx_tmem_source_stamp(f.srcStamp); /* before any of the run's loads read a source */
    R.unret = false;
    R.unretWhy = 0;
    R.unretDetail = 0;
    R.impEntered0 = mStats.texImports;
    R.impNoted = 0;
    R.combFlush0 = mStats.flush[FLUSH_COMBINER];
    R.curStartTri = static_cast<uint32_t>(mBufNumTris);
    R.curSerial = 0;
    memset(R.storedWords, 0, sizeof(R.storedWords));
    R.cur = &rec;
    R.curSlot = slot;
    R.curSet = set;
    R.curWay = static_cast<uint32_t>(way);
    f.shape = shape;
    R.verifying = hit; /* mHudRetain == 2 here */
    if (GDX_UNLIKELY(R.verifying)) {
        HudRetainVerifyPrepare(rec);
    }
    R.realBackend = mBackend;
    R.recBackend->real = mBackend;
    mBackend = R.recBackend;
    mHudRec = true;
    if (GDX_UNLIKELY(clk2)) {
        R.wTicksRecBegin += static_cast<uint64_t>(gdx_ninterp_now()) - c0;
    }
    return 0;
}

namespace {

/* == DpFillRectangle's branch: 0 draws, 1 draws nothing, 2 clears a depth region. */
uint8_t FillPath(const GdxHudWord& w, bool zeq, uint32_t nativeW, uint32_t nativeH) {
    const int32_t ulx = static_cast<int32_t>(C1(w.w1, 12, 12));
    const int32_t uly = static_cast<int32_t>(C1(w.w1, 0, 12));
    const int32_t lrx = static_cast<int32_t>(C0(w.w0, 12, 12));
    const int32_t lry = static_cast<int32_t>(C0(w.w0, 0, 12));
    if (zeq) {
        const bool full = ulx <= 0 && uly <= 0 && lrx >= static_cast<int32_t>(nativeW - 1) * 4 &&
                          lry >= static_cast<int32_t>(nativeH - 1) * 4;
        return full ? 1 : 2;
    }
    return (ulx > lrx || uly > lry) ? 1 : 0;
}

} // namespace

void Interp::HudRecOpPre(uint32_t wordIndex, uint8_t op) {
    (void)op;
    mHudR->curWord = wordIndex;
    mHudR->preSerial = mHudR->curSerial;
    mHudR->preEmit = mStats.triEmitAll;
    HudRetRec& f = *mHudR->fresh;
    if (wordIndex < f.opOpen.n) {
        f.opOpen.d[wordIndex] = mBufNumTris > 0 ? 1 : 0;
        f.opTile.d[wordIndex] = mRdp.first_tile_index;
        if (op == OP_FILLRECT) {
            f.opFill.d[wordIndex] = FillPath(f.words.d[wordIndex], mRdp.color_image_address == mRdp.z_buf_address,
                                             mCfg.nativeW, mCfg.nativeH);
        }
        if (op == OP_SETPRIMCOLOR || op == OP_SETENVCOLOR || op == OP_SETFOGCOLOR || op == OP_SETBLENDCOLOR ||
            op == OP_SETPRIMDEPTH) {
            f.opPred.d[wordIndex] = HudColourSame(op, f.words.d[wordIndex].w0, f.words.d[wordIndex].w1) ? 1 : 2;
        }
        mHudR->preBatches = mStats.batches;
    }
}

void Interp::HudRecOpPost(uint32_t wordIndex, uint8_t op) {
    HudRetRec& f = *mHudR->fresh;
    if (wordIndex < f.opFlush.n) {
        f.opFlush.d[wordIndex] = mStats.batches != mHudR->preBatches ? 1 : 0;
    }
    if (op == OP_TEXRECT || op == OP_TEXRECTFLIP || op == OP_FILLRECT) {
        f.rects++;
        const bool draws = op != OP_FILLRECT || (wordIndex < f.opFill.n && f.opFill.d[wordIndex] == 0);
        if (draws && mStats.triEmitAll - mHudR->preEmit != 2u) {
            mHudR->Unret(RU_REJECT); /* a rejected rectangle: its state block did not run */
        }
        if (!draws) {
            return;
        }
        if (wordIndex < f.opTex.n && mTriMemoValid && mTriMemo.comb != nullptr) {
            f.opTex.d[wordIndex] = static_cast<uint8_t>(0x80u | (mTriMemo.usedTextures[0] ? 1u : 0u) |
                                                        (mTriMemo.usedTextures[1] ? 2u : 0u) |
                                                        (DrawReadsVertexColour(mTriMemo) ? 0x40u : 0u) |
                                                        (mTriMemo.use_fog && mTriMemo.use_blend_color ? 0x20u : 0u));
            f.opRegs.d[wordIndex] = DrawRegs(mTriMemo);
        }
        if (mHudR->wantCtx) {
            HudRecNoteRect(wordIndex, op);
        }
    } else if (op == OP_LOADBLOCK) {
        HudRecNoteLoad();
    } else if (op == OP_LOADTILE || op == OP_LOADTLUT || op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD) {
        mHudR->Unret(RU_OP);
    }
}

/* A rectangle of the recorded run: where its triangles went, and what they were computed from. */
void Interp::HudRecNoteRect(uint32_t wordIndex, uint8_t op) {
    HudRetain& R = *mHudR;
    RetRect rc;
    memset(&rc, 0, sizeof(rc));
    rc.word = static_cast<uint16_t>(wordIndex);
    rc.op = op;
    const uint32_t emitted = mStats.triEmitAll - R.preEmit;
    const bool began = R.curSerial != R.preSerial;
    rc.ok = emitted == 2u && mPackBatch && mBufNumTris >= 2u && mTriMemoValid && mTriMemo.comb != nullptr &&
                    R.curSerial - R.preSerial <= 1u && (!began || mBufNumTris == 2u)
                ? 1
                : 0;
    rc.began = began ? 1 : 0;
    rc.serial = static_cast<uint16_t>(R.curSerial);
    rc.tri0 = static_cast<uint16_t>(mBufNumTris >= 2u ? mBufNumTris - 2u : 0u);
    rc.omL = mRdp.other_mode_l;
    rc.omH = mRdp.other_mode_h;
    rc.xgeom = mRsp.extra_geometry_mode;
    rc.ds = mTriMemo;
    rc.params = mPackParams;
    rc.tiles[0] = mRdp.texture_tile[mTriMemo.effective_tile[0] & 7u];
    rc.tiles[1] = mRdp.texture_tile[mTriMemo.effective_tile[1] & 7u];
    rc.prim = mRdp.prim_color;
    rc.env = mRdp.env_color;
    rc.fog = mRdp.fog_color;
    rc.blend = mRdp.blend_color;
    rc.keyC = mRdp.key_center;
    rc.keyS = mRdp.key_scale;
    rc.primLod = mRdp.prim_lod_fraction;
    memcpy(rc.conv, mRdp.convert_k, sizeof(rc.conv));
    memcpy(rc.lv, &mRsp.loaded_vertices[kMaxVertices], sizeof(rc.lv));
    rc.trectOk = op != OP_FILLRECT && mHudLastTrectOk ? 1 : 0;
    if (!R.fresh->rectCtx.Push(rc)) {
        R.Unret(RU_ALLOC);
    }
}

/* A LOADBLOCK of the recorded run: its resolved form (HudDirectLoadBlock left it). */
void Interp::HudRecNoteLoad() {
    HudRetain& R = *mHudR;
    const HudLoadForm* form = HudDirectLastLoad();
    const bool skipped = form != nullptr && form->skipped != 0;
    if (form == nullptr || form->valid == 0 || form->loaded.addr == nullptr ||
        (!skipped && (form->copyBytes == 0 || form->cls == GDX_TMEMSRC_UNKNOWN))) {
        if (form != nullptr && !R.unret) {
            R.unretDetail = (form->valid != 0 ? 1u : 0u) | (form->imm != 0 ? 2u : 0u) |
                            (form->copyBytes == 0 ? 4u : 0u) | (form->loaded.addr == nullptr ? 8u : 0u) |
                            (static_cast<uint32_t>(form->cls) << 8);
        }
        R.Unret(RU_LOAD);
        return;
    }
    if (!R.fresh->loads.Push(*form)) {
        R.Unret(RU_ALLOC);
        return;
    }
    if (skipped) {
        return; /* nothing stored: the span it matched is the one an import samples */
    }
    /* the TMEM words this span store now owns (an import may only sample these) */
    const uint32_t logical = form->loaded.orig_size_bytes != 0 ? form->loaded.orig_size_bytes : form->loaded.size_bytes;
    uint32_t words = (logical + 7u) / 8u;
    if (words < 1) {
        words = 1;
    }
    for (uint32_t k = form->tmemIndex; k < form->tmemIndex + words && k < 512u; k++) {
        R.storedWords[k >> 6] |= 1ull << (k & 63u);
    }
}

namespace {

bool StoredBit(const uint64_t* bits, uint32_t w) {
    return w < 512u && ((bits[w >> 6] >> (w & 63u)) & 1u) != 0;
}

} // namespace

void Interp::HudRecNoteImportBegin(uint16_t tmemIdx, uint32_t hashOff, uint32_t hashSpan, bool tmemHashed, bool covered,
                                   bool supported) {
    HudRetain& R = *mHudR;
    const uint16_t bp1 = tmemIdx < 512u ? mRdp.tmem_span_base[tmemIdx] : 0;
    const bool ownWord = StoredBit(R.storedWords, tmemIdx);
    const bool ownBase = bp1 != 0 && StoredBit(R.storedWords, bp1 - 1u);
    const bool immutable = bp1 != 0 && mRdp.loaded_texture[bp1 - 1u].immutable != 0;
    bool ok = supported && (!tmemHashed || covered) && bp1 != 0;
    if (ok && !(ownWord && ownBase)) {
        /* a span an earlier run stored: fingerprinted by its descriptor and (TMEM-hashed) its
         * owed source; an immutable span's key is its constant source bytes */
        ok = !ownWord && !ownBase && ((tmemHashed && covered) || immutable);
        if (ok) {
            RetForeign fo;
            memset(&fo, 0, sizeof(fo));
            fo.word = tmemIdx;
            fo.base = static_cast<uint16_t>(bp1 - 1u);
            fo.spanWords = mRdp.tmem_span_words[fo.base];
            fo.kind = mRdp.tmem_span_load_kind[fo.base];
            fo.lt = mRdp.loaded_texture[fo.base];
            fo.off = hashOff;
            fo.span = hashSpan;
            if (tmemHashed) {
                HudDirectLastHashSource(&fo.src, &fo.native, &fo.cls);
                ok = fo.src != nullptr && fo.cls != GDX_TMEMSRC_UNKNOWN;
            }
            ok = ok && R.fresh->foreign.Push(fo);
        }
    }
    if (!ok) {
        R.Unret(RU_IMPORT);
    }
}

void Interp::HudRecNoteImportHit(int unit, int16_t slot, bool uploaded) {
    HudRetain& R = *mHudR;
    R.impNoted++;
    if (slot < 0 || !uploaded) {
        R.Unret(RU_IMPORT);
        return;
    }
    RetImp im;
    memset(&im, 0, sizeof(im));
    im.slot = slot;
    im.unit = static_cast<uint8_t>(unit);
    im.texId = mTex[slot].texture_id;
    memcpy(&im.key, &mTex[slot].key, sizeof(TexKey));
    if (!R.fresh->imps.Push(im)) {
        R.Unret(RU_ALLOC);
    }
}

/* The recorded run finished executing: close the record (and under verify, compare it with
 * the retained one it would have replayed). */
void Interp::HudRetainEnd() {
    InterpHwsScope hws(GDX3DS_HWS_HUDREC);
    HudRetain& R = *mHudR;
    const uint64_t e0 = mHudRetClock != 0 ? static_cast<uint64_t>(gdx_ninterp_now()) : 0;
    mBackend = R.realBackend;
    mHudRec = false;
    HudRetRec& f = *R.fresh;
    /* the batch left open: its floats written by this run */
    if (mBufNumTris > 0) {
        R.recBackend->CaptureFloats(mBufNumTris);
        f.tailBatchBegan = f.began ? 1 : 0;
    }
    if (mStats.texImports - R.impEntered0 != R.impNoted) {
        R.Unret(RU_IMPORT);
    }
    if (mStats.flush[FLUSH_COMBINER] != R.combFlush0) {
        R.Unret(RU_COMB);
    }
    HudRegCapture(f.exit);
    uint64_t cmp = 0, wr = 0;
    if (!R.unret && !HudRetainAnalyse(f, &cmp, &wr)) {
        R.Unret(RU_OP);
    }
    f.cmpMask = cmp;
    f.writeMask = wr;
    /* the distinct sources the stored loads read (one generation check each per hit) */
    for (uint32_t k = 0; k < f.loads.n && !R.unret; k++) {
        const HudLoadForm& ld = f.loads.d[k];
        if (ld.skipped != 0 || ld.copyBytes == 0) {
            continue;
        }
        bool seen = false;
        for (uint32_t j = 0; j < f.srcs.n && !seen; j++) {
            RetSrc& sr = f.srcs.d[j];
            if (sr.addr == ld.loaded.addr && sr.cls == ld.cls) {
                sr.n = sr.n > ld.copyBytes ? sr.n : ld.copyBytes;
                seen = true;
            }
        }
        const RetSrc sr = { ld.loaded.addr, ld.copyBytes, ld.cls };
        if (!seen && !f.srcs.Push(sr)) {
            R.Unret(RU_ALLOC);
        }
    }
    HudRetRec& rec = *R.cur;
    if (GDX_UNLIKELY(R.verifying)) {
        HudRetainVerifyCompare(rec);
    }
    if (R.unret && R.unretWhy == RU_OP && !R.verifying) {
        /* keep the words: while they stay the same the run is not recorded again */
        f.valid = false;
        f.structUnret = true;
        HudRetRec* done = R.fresh;
        R.fresh = R.cur;
        R.rec[R.curSet][R.curWay] = done;
    }
    if (R.unret) {
        R.wUnret[R.unretWhy]++;
        rec.valid = false;
        if (mHudElem != 0 && (R.taskSeq % kElemEvery) == 0) {
            [&R, &f]() GDX_COLD {
            const uint32_t at = R.unretAt < f.nWords ? R.unretAt : 0;
            GDXN_LOG("[hudr] unret task=%u run=%u words=%u why=%s at=%u op=%02X detail=%X", R.taskSeq, R.curSlot,
                     f.nWords, kUnretName[R.unretWhy], R.unretAt, f.nWords != 0 ? Opcode(f.words.d[at].w0) : 0u,
                     R.unretDetail);
            }();
        }
    } else {
        f.valid = true;
        HudRetRec* done = R.fresh;
        R.fresh = R.cur;
        R.rec[R.curSet][R.curWay] = done;
        R.wRecorded++;
    }
    R.cur = nullptr;
    R.verifying = false;
    if (GDX_UNLIKELY(mHudRetClock != 0)) {
        R.wTicksRecEnd += static_cast<uint64_t>(gdx_ninterp_now()) - e0;
    }
}

/* ---- verify -------------------------------------------------------------------------------- */

namespace {

void SpanCapture(SpanSnap& s, const Rdp& rdp, uint32_t spanEpoch) {
    memcpy(s.lt, rdp.loaded_texture, sizeof(s.lt));
    memcpy(s.base, rdp.tmem_span_base, sizeof(s.base));
    memcpy(s.words, rdp.tmem_span_words, sizeof(s.words));
    memcpy(s.kind, rdp.tmem_span_load_kind, sizeof(s.kind));
    memcpy(s.stamp, rdp.tmem_span_src_stamp, sizeof(s.stamp));
    memcpy(s.cls, rdp.tmem_span_src_cls, sizeof(s.cls));
    memcpy(s.list, rdp.tmem_span_list, sizeof(s.list));
    s.listN = rdp.tmem_span_list_n;
    s.tmemGen = rdp.tmem_generation;
    s.spanEpoch = spanEpoch;
}

void SpanRestore(const SpanSnap& s, Rdp& rdp, uint32_t* spanEpoch) {
    memcpy(rdp.loaded_texture, s.lt, sizeof(s.lt));
    memcpy(rdp.tmem_span_base, s.base, sizeof(s.base));
    memcpy(rdp.tmem_span_words, s.words, sizeof(s.words));
    memcpy(rdp.tmem_span_load_kind, s.kind, sizeof(s.kind));
    memcpy(rdp.tmem_span_src_stamp, s.stamp, sizeof(s.stamp));
    memcpy(rdp.tmem_span_src_cls, s.cls, sizeof(s.cls));
    memcpy(rdp.tmem_span_list, s.list, sizeof(s.list));
    rdp.tmem_span_list_n = s.listN;
    rdp.tmem_generation = s.tmemGen;
    *spanEpoch = s.spanEpoch;
}

/* The span model as the stock path leaves it, compared field by field where it is live. */
bool SpanSame(const SpanSnap& a, const SpanSnap& b) {
    if (a.tmemGen != b.tmemGen || a.spanEpoch != b.spanEpoch || memcmp(a.base, b.base, sizeof(a.base)) != 0 ||
        memcmp(a.words, b.words, sizeof(a.words)) != 0 || memcmp(a.kind, b.kind, sizeof(a.kind)) != 0 ||
        memcmp(a.cls, b.cls, sizeof(a.cls)) != 0 || memcmp(a.stamp, b.stamp, sizeof(a.stamp)) != 0 ||
        a.listN != b.listN) {
        return false;
    }
    for (uint32_t i = 0; i < 512u; i++) {
        const LoadedTexture& x = a.lt[i];
        const LoadedTexture& y = b.lt[i];
        if (x.addr != y.addr || x.orig_size_bytes != y.orig_size_bytes || x.size_bytes != y.size_bytes ||
            x.full_image_line_size_bytes != y.full_image_line_size_bytes || x.line_size_bytes != y.line_size_bytes ||
            x.tmem_start != y.tmem_start || x.tmem_word_count != y.tmem_word_count || x.immutable != y.immutable ||
            x.native_rgba16 != y.native_rgba16) {
            return false;
        }
    }
    const uint32_t n = a.listN == 0xFFFFu ? 0u : a.listN;
    for (uint32_t i = 0; i < n; i++) {
        bool found = false;
        for (uint32_t j = 0; j < n && !found; j++) {
            found = a.list[i] == b.list[j];
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

} // namespace

/* Before the verify execution: predict the span model and TMEM the hit would leave (the recorded
 * loads applied to the entry state), then put the entry state back. */
GDX_COLD void Interp::HudRetainVerifyPrepare(HudRetRec& rec) {
    HudRetain& R = *mHudR;
    if (R.vfySpanPred == nullptr) {
        R.vfySpanPred = static_cast<SpanSnap*>(malloc(sizeof(SpanSnap)));
        R.vfySpanTmp = static_cast<SpanSnap*>(malloc(sizeof(SpanSnap)));
        R.vfyLedgerTmp = static_cast<uint8_t*>(malloc(HudDebtStateBytes()));
    }
    if (R.vfySpanPred == nullptr || R.vfySpanTmp == nullptr || R.vfyLedgerTmp == nullptr) {
        R.verifying = false;
        return;
    }
    HudRegCapture(R.vfyEntry);
    SpanCapture(*R.vfySpanTmp, mRdp, mSpanEpoch);
    HudDebtStateSave(R.vfyLedgerTmp);
    for (uint32_t k = 0; k < rec.loads.n; k++) {
        HudDirectApplyLoad(rec.loads.d[k], false);
    }
    SpanCapture(*R.vfySpanPred, mRdp, mSpanEpoch);
    HudDebtMaterialize(R.vfyTmemPred);
    SpanRestore(*R.vfySpanTmp, mRdp, &mSpanEpoch);
    HudDebtStateLoad(R.vfyLedgerTmp);
    /* ApplyLoad armed textures_changed: the entry registers go back too */
    mRdp.textures_changed[0] = R.vfyEntry.txc[0];
    mRdp.textures_changed[1] = R.vfyEntry.txc[1];
}

GDX_COLD void Interp::HudRetainVerifyCompare(const HudRetRec& rec) {
    HudRetain& R = *mHudR;
    const HudRetRec& f = *R.fresh;
    R.wVfyRuns++;
    R.sesVfyRuns++;
    bool bad = false;
    int where = -1;
    char what[48] = "";
    if (R.unret) {
        R.wVfyUnret++; /* the stock execution was not retainable although the hit would have been */
        bad = true;
        snprintf(what, sizeof(what), "unret:%s", kUnretName[R.unretWhy]);
    }
    /* the backend log */
    if (!bad && (f.evs.n != rec.evs.n || f.floats.n != rec.floats.n)) {
        R.wVfyBadEv++;
        bad = true;
        snprintf(what, sizeof(what), "evs %u/%u floats %u/%u", f.evs.n, rec.evs.n, f.floats.n, rec.floats.n);
    }
    for (uint32_t k = 0; !bad && k < f.evs.n; k++) {
        const RetEv& a = f.evs.d[k];
        const RetEv& b = rec.evs.d[k];
        bool same = SameEv(a, b);
        if (!same && a.op == b.op && a.c == b.c && b.c != 0 && (b.op == EV_PRIMDEPTH || b.op == EV_ALPHATHR)) {
            /* a live-bound drain value: what the hit would pass is the entry register's */
            const float live = b.op == EV_PRIMDEPTH ? static_cast<float>(R.vfyEntry.pdepth) / 32767.0f
                                                    : static_cast<float>(R.vfyEntry.blend.a) / 255.0f;
            same = a.i[3] == b.i[3] && SameBytes(&a.f, &live, sizeof(float));
        }
        if (same && a.op == EV_BEGIN) {
            same = SameParams(f.params.d[a.i[0]], rec.params.d[b.i[0]]);
        } else if (same && a.op == EV_REFRESH) {
            same = SameParams(f.params.d[a.i[0]], rec.params.d[b.i[0]]);
        } else if (same && a.op == EV_DRAW) {
            same = SameAux(f.auxs.d[a.i[1]], rec.auxs.d[b.i[1]]);
        }
        if (!same) {
            R.wVfyBadEv++;
            bad = true;
            where = static_cast<int>(k);
            snprintf(what, sizeof(what), "ev%u op=%u/%u", k, a.op, b.op);
        }
    }
    if (!bad && memcmp(f.floats.d, rec.floats.d, sizeof(float) * f.floats.n) != 0) {
        R.wVfyBadFloat++;
        bad = true;
        snprintf(what, sizeof(what), "floats");
    }
    /* the registers: written groups as recorded, the others untouched */
    HudRegs live;
    HudRegCapture(live);
    for (int g = 0; !bad && g < RG_COUNT; g++) {
        const bool written = ((rec.writeMask >> g) & 1u) != 0;
        bool same;
        if (g == RG_PACK) {
            same = live.bufNumTris == (written ? rec.exit.bufNumTris : R.vfyEntry.bufNumTris);
            if (same && written) {
                same = live.packBatch == rec.exit.packBatch && live.packCap == rec.exit.packCap &&
                       live.packComb == rec.exit.packComb && live.packFolded == rec.exit.packFolded &&
                       (!rec.paramsWritten || SameParams(live.params, rec.exit.params)) &&
                       (!rec.auxWritten || SameAux(live.aux, rec.exit.aux)) &&
                       rec.paramsWritten == f.paramsWritten && rec.auxWritten == f.auxWritten;
            }
        } else {
            same = HudRegSame(live, written ? rec.exit : R.vfyEntry, g);
        }
        if (!same) {
            R.wVfyBadReg++;
            bad = true;
            where = g;
            snprintf(what, sizeof(what), "reg %s (%s)", kGroupName[g], written ? "written" : "kept");
        }
    }
    /* the span model and TMEM */
    if (!bad) {
        SpanCapture(*R.vfySpanTmp, mRdp, mSpanEpoch);
        if (!SpanSame(*R.vfySpanTmp, *R.vfySpanPred)) {
            R.wVfySpan++;
            bad = true;
            snprintf(what, sizeof(what), "span");
        }
    }
    if (!bad) {
        HudDebtMaterialize(R.vfyTmemLive);
        if (memcmp(R.vfyTmemLive, R.vfyTmemPred, sizeof(R.vfyTmemLive)) != 0) {
            R.wVfyTmem++;
            bad = true;
            snprintf(what, sizeof(what), "tmem");
        }
    }
    /* the cache: the same slots hit, and the LRU tail is them in last-touch order */
    if (!bad && f.imps.n != rec.imps.n) {
        R.wVfyLru++;
        bad = true;
        snprintf(what, sizeof(what), "imps %u/%u", f.imps.n, rec.imps.n);
    }
    for (uint32_t k = 0; !bad && k < f.imps.n; k++) {
        if (f.imps.d[k].slot != rec.imps.d[k].slot) {
            R.wVfyLru++;
            bad = true;
            snprintf(what, sizeof(what), "imp%u slot", k);
        }
    }
    if (!bad && rec.imps.n != 0) {
        int16_t node = mTexLruTail;
        for (int k = static_cast<int>(rec.imps.n) - 1; k >= 0 && !bad; k--) {
            bool later = false; /* touched again after k: its LRU position is the later touch's */
            for (uint32_t j = static_cast<uint32_t>(k) + 1u; j < rec.imps.n; j++) {
                later = later || rec.imps.d[j].slot == rec.imps.d[k].slot;
            }
            if (later) {
                continue;
            }
            if (node != rec.imps.d[k].slot) {
                R.wVfyLru++;
                bad = true;
                snprintf(what, sizeof(what), "lru");
                break;
            }
            node = mTex[node].lruPrev;
        }
    }
    if (bad) {
        R.wVfyBad++;
        R.sesVfyBad++;
        if (R.vfyLogged < 16) {
            R.vfyLogged++;
            GDXN_LOG("[hudr] VERIFY MISMATCH task=%u run=%u words=%u: %s (at %d)", R.taskSeq, R.curSlot, rec.nWords,
                     what, where);
        }
    } else {
        R.wVfyOk++;
    }
}

/* ---- the task end and the receipt ------------------------------------------------------------ */

void Interp::HudRetainTaskEnd() {
    if (mHudR == nullptr) {
        return;
    }
    HudRetain& R = *mHudR;
    if (R.runInTask != 0) {
        R.winTasks++;
    }
    const uint32_t k = R.runInTask < kRunSlots ? R.runInTask : kRunSlots;
    memcpy(R.prevHash, R.curHash, sizeof(uint32_t) * k);
    memcpy(R.prevLen, R.curLen, sizeof(uint32_t) * k);
    for (uint32_t i = k; i < kRunSlots; i++) {
        R.prevHash[i] = 0;
        R.prevLen[i] = 0xFFFFFFFFu;
    }
    R.runInTask = 0;
    R.taskSeq++;
}

/* The run's slot advances once per HUD run, whatever path it took. */
void Interp::HudRetainRunDone() {
    if (mHudR != nullptr) {
        mHudR->runInTask++;
    }
}

GDX_COLD int Interp::HudElemFormat(char* buf, size_t cap, uint32_t tasks) {
    if (mHudR == nullptr || cap == 0 || tasks == 0) {
        return 0;
    }
    HudRetain& R = *mHudR;
    int n = 0;
    if (R.winTasks != 0 && mHudElem != 0) {
        const double inv = 1.0 / static_cast<double>(R.winTasks);
        n = snprintf(buf, cap, "[hudelem] sum tasks=%u hudtasks=%u runs=%.2f same=%.2f words=%.1f",
                     static_cast<unsigned>(tasks), R.winTasks, R.winRuns * inv, R.winSame * inv, R.winWords * inv);
    }
    R.winRuns = R.winSame = R.winWords = 0;
    return n;
}

/* [hudr] per HUD task (the receipt window): runs, hits (replayed), recorded; what the hits
 * saved (rects, loads, imports not executed; draws and triangles re-submitted); why the other
 * runs missed (the regs miss names the first differing register group); why a recorded run was
 * not retainable; under verify the comparison of every would-be hit (bad MUST be 0). */
GDX_COLD int Interp::HudRetainFormat(char* buf, size_t cap, uint32_t tasks) {
    if (mHudR == nullptr || cap == 0 || tasks == 0) {
        return 0;
    }
    HudRetain& R = *mHudR;
    int n = 0;
    if (mHudRetain != 0 && R.wRuns != 0) {
        const double inv = 1.0 / static_cast<double>(R.winTasks != 0 ? R.winTasks : tasks);
        n += snprintf(buf + n, cap - static_cast<size_t>(n),
                      "[hudr] tasks=%u mode=%d runs=%.2f hit=%.2f rec=%.2f | hit rects=%.1f loads=%.1f imps=%.1f "
                      "draws=%.1f tris=%.1f cool=%.2f | variant hit=%.2f moved=%.1f no=%.2f selfbad=%u | miss",
                      static_cast<unsigned>(tasks), mHudRetain, R.wRuns * inv, R.wHit * inv, R.wRecorded * inv,
                      R.wHitRects * inv, R.wHitLoads * inv, R.wHitImps * inv, R.wHitDraws * inv, R.wHitTris * inv,
                      R.wCool * inv, R.wVarHit * inv, R.wVarRects * inv, R.wVarNo * inv, R.wVarSelfBad);
        if (R.wVarNo != 0 && static_cast<size_t>(n) < cap) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n), " (why fp=%u map=%u fill=%u tile=%u unp=%u off=%u self=%u)",
                          R.wVarWhy[0], R.wVarWhy[1], R.wVarWhy[2], R.wVarWhy[3], R.wVarWhy[4], R.wVarWhy[5],
                          R.wVarWhy[6]);
        }
        for (int k = 1; k < RM_COUNT && static_cast<size_t>(n) < cap; k++) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n), " %s=%.2f", kMissName[k], R.wMiss[k] * inv);
        }
        if (R.wMiss[RM_REGS] != 0 && static_cast<size_t>(n) < cap) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n), " (");
            for (int g = 0; g < RG_COUNT && static_cast<size_t>(n) < cap; g++) {
                if (R.wRegMiss[g] != 0) {
                    n += snprintf(buf + n, cap - static_cast<size_t>(n), " %s=%u", kGroupName[g], R.wRegMiss[g]);
                }
            }
            if (static_cast<size_t>(n) < cap) {
                n += snprintf(buf + n, cap - static_cast<size_t>(n), " )");
            }
        }
        if (static_cast<size_t>(n) < cap) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n), " | unret");
        }
        for (int k = 0; k < RU_COUNT && static_cast<size_t>(n) < cap; k++) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n), " %s=%u", kUnretName[k], R.wUnret[k]);
        }
        if (mHudRetain == 2 && static_cast<size_t>(n) < cap) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n),
                          " | vfy runs=%u ok=%u bad=%u (ev=%u float=%u reg=%u span=%u tmem=%u lru=%u unret=%u) "
                          "ses=%u/%u",
                          R.wVfyRuns, R.wVfyOk, R.wVfyBad, R.wVfyBadEv, R.wVfyBadFloat, R.wVfyBadReg, R.wVfySpan,
                          R.wVfyTmem, R.wVfyLru, R.wVfyUnret, R.sesVfyBad, R.sesVfyRuns);
        }
        if ((mHudClock != 0 || mHudRetClock != 0) && static_cast<size_t>(n) < cap) {
            const double toMs = 1.0 / 268111.856 / static_cast<double>(tasks);
            n += snprintf(buf + n, cap - static_cast<size_t>(n), " | clock hitms=%.3f",
                          static_cast<double>(R.wTicksHit) * toMs);
            if (mHudRetClock != 0 && static_cast<size_t>(n) < cap) {
                n += snprintf(buf + n, cap - static_cast<size_t>(n),
                              " fp=%.3f var=%.3f replay=%.3f (loads=%.3f ev=%.3f apply=%.3f) recbeg=%.3f recend=%.3f",
                              static_cast<double>(R.wTicksFp) * toMs, static_cast<double>(R.wTicksVar) * toMs,
                              static_cast<double>(R.wTicksReplay) * toMs, static_cast<double>(R.wTicksRLoad) * toMs,
                              static_cast<double>(R.wTicksREv) * toMs, static_cast<double>(R.wTicksRApply) * toMs,
                              static_cast<double>(R.wTicksRecBegin) * toMs, static_cast<double>(R.wTicksRecEnd) * toMs);
                if (mHudRetClock >= 2 && static_cast<size_t>(n) < cap) {
                    n += snprintf(buf + n, cap - static_cast<size_t>(n), " fpph regs=%.3f tex=%.3f plan=%.3f src=%.3f",
                                  static_cast<double>(R.wTicksFpPh[0]) * toMs, static_cast<double>(R.wTicksFpPh[1]) * toMs,
                                  static_cast<double>(R.wTicksFpPh[2]) * toMs, static_cast<double>(R.wTicksFpPh[3]) * toMs);
                }
            }
        }
    }
    R.winTasks = 0;
    R.wRuns = R.wHit = R.wRecorded = 0;
    memset(R.wMiss, 0, sizeof(R.wMiss));
    memset(R.wUnret, 0, sizeof(R.wUnret));
    memset(R.wRegMiss, 0, sizeof(R.wRegMiss));
    R.wHitRects = R.wHitLoads = R.wHitImps = R.wHitDraws = R.wHitTris = 0;
    R.wVfyRuns = R.wVfyOk = R.wVfyBad = R.wVfyBadEv = R.wVfyBadFloat = R.wVfyBadReg = 0;
    R.wVfySpan = R.wVfyTmem = R.wVfyLru = R.wVfyUnret = 0;
    R.wTicksHit = R.wTicksExec = 0;
    R.wTicksFp = R.wTicksVar = R.wTicksRecBegin = R.wTicksRecEnd = R.wTicksReplay = 0;
    R.wTicksRLoad = R.wTicksREv = R.wTicksRApply = 0;
    memset(R.wTicksFpPh, 0, sizeof(R.wTicksFpPh));
    R.wVarHit = R.wVarRects = R.wVarSelfBad = R.wVarNo = 0;
    R.wCool = 0;
    memset(R.wVarWhy, 0, sizeof(R.wVarWhy));
    return n < static_cast<int>(cap) ? n : static_cast<int>(cap) - 1;
}

/* [native] hudsky_emit: the compact emitter for retained runs (hudsky). */
#include "gdx_hudsky_emit.inc"

} // namespace gdxn
