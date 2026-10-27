/* gdx3ds_track_retained.cpp — [native] track_retained: the course's material runs as
 * GPU-transformed draws (NEXT.md C5 Stage 3; design o3ds60-design.md §4, as built in
 * c5s3-progress.md). Levers and receipts: gdx3ds_track_retained.h.
 *
 * What the CPU path does per run (gdx3ds_track_replay.cpp): every recorded vertex load is a
 * full interpreter T&L (SpVertex: transform, aspect, UV scale, colour, fog, clip flags), and
 * every template triangle is screened, culled, clipped when it crosses the frustum and packed
 * as 3 x 12 floats. What this path does instead:
 *
 *  - Vertex loads only LATCH the slot (source pointer, texture scale in force, ST patches).
 *  - A triangle headed for the GPU needs its three positions screened on the CPU (the exact
 *    fp32 expressions of the interpreter, so reject / all-out / clip decisions are the CPU
 *    path's own) and its three vertices appended to the run's stream: the game's 16-byte Vtx
 *    with ob rebased to the pass origin (an integer point at the eye: exact s16 subtraction)
 *    and the FINAL short U/V the interpreter would have computed (the stream's tc; the shader
 *    runs with texture scale 1). The PICA then runs shader.v.pica's gpuxf proc unchanged
 *    (lighting, texgen and the FLX select off) with the rebased matrix.
 *  - The first triangle of every material segment still takes the interpreter's exact path:
 *    it resolves the draw state (combiner, texture import, sampler, aux constants) the way the
 *    CPU path does, and the fast-path prep derived from it is what the GPU uniforms are built
 *    from -- so the UV affine, colour/alpha selects and fog are the CPU path's parameters.
 *  - A triangle the float24 gate refuses, whose rebased vertex leaves s16, or (parity mode)
 *    that the CPU clipper would split, is drawn by the CPU path in place: the GPU list is
 *    flushed first, its slots are materialised in the interpreter's cache on demand, and the
 *    existing exact/fast path draws it. Painter's order is kept by construction.
 *
 * float24. Relative to the origin E (the eye rounded to integers), |ob - E| is at most the
 * visible course extent, and the composed matrix's translation row is small: the terms of the
 * dp4 no longer cancel. The per-triangle bound is gpuxf's (o3dsgpuxf-progress.md §2):
 * e = 2^-14 (sum_k |m_k| |ob_k| + |m_3|) per row, (e_x + e_w) / w in screen pixels, with w
 * floored at the near plane's w for a triangle the PICA will clip there. */
#include "gdx3ds_track_retained.h"

#include <3ds.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gdx3ds_interp_seam.h"
#include "gdx3ds_mesh.h"
#include "gdx3ds_native_tri.h"
#include "gdx3ds_nativebatch.h"
#include "gdx3ds_track_record.h"
#include "gdx3ds_track_resident.h"
#include "gdx3ds_track_retained_math.h"
#include "gdx3ds_track_templates.h"
#include "gdx3ds_vtx_math.h"
#include "gdx3ds_xf_census.h"

extern "C" void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" int gdx3ds_config_loaded(void) __attribute__((weak));
extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact);
extern "C" int gdx3ds_c5cat_armed(void) __attribute__((weak));

using GdxNativeTri::RunCtx;
using GdxTrkRr::BoundV;
using GdxTrkRr::ClipPoly;
using GdxTrkRr::ClipXf;
using GdxTrkRr::NearPlaneW;
using GdxTrkRr::PlaneDist;
using GdxTrkRr::PolyV;
using GdxTrkRr::SolveEye;
using GdxTrkRr::VisibleBoundMpx;

namespace {

constexpr int kSlots = 32;         /* the track loads slots 0..20; the recorder refuses v0 + n > 32 */
constexpr uint32_t kMaxIdx = 3072; /* per GPU draw; a longer segment is split */
constexpr float kF24Row = 1.0f / 16384.0f;
/* Near-plane polygons ([native] track_retained=1): clipped on the CPU in rebased object space at
 * z + w = 0 (the CPU clipper's near plane) and drawn with float3 positions and float2 texture
 * coordinates (the backend's 24-byte gpuxf layout) by the SAME matrix as the coarse draw: an
 * unclipped corner is the same integer in both layouts, so its float24 transform is bit-identical
 * and the edges the two draws share seal. The texture coordinates are the interpreter's short U/V
 * times kFineUv (the shader floors U before its affine; the fine draw's affine slope is divided
 * by kFineUv, a power of two, so an unclipped corner's UV is bit-identical too). */
constexpr float kFineUv = 16.0f;
constexpr uint32_t kFineChunk = 96;   /* fine-stream vertices per pool reservation */
constexpr uint32_t kMaxFineIdx = 288; /* 3 * kFineChunk */

/* The backend's 24-byte gpuxf vertex (gfx_citro3d.cpp mAttrXfF). */
struct FineVtx {
    float p[3];
    float tc[2];
    uint8_t cn[4];
};
static_assert(sizeof(FineVtx) == 24, "the float gpuxf layout is 24 bytes");

enum Level { LEVEL_OFF = 0, LEVEL_PERF = 1, LEVEL_PARITY = 2 };

struct Config {
    bool latched;
    int level;
    int maxErr; /* 1/1000 px; < 0 = no gate */
    bool rebase;
    bool sideClipAlways; /* [native] track_retained_sideclip=1 */
    int matTab;          /* [native] track_mattab: 0 off, 1 use, 2 verify (C5 Stage 3d) */
    bool matLive;        /* [native] track_mattab_live: a unit the record holds no import hit for imports live */
    int nearClip;        /* [native] track_nearclip: 0 stock ClipTri, 1 lean (bit-identical) */
    int opFold;          /* [native] track_opfold: 0 per-triangle resident loop, 1 folded template append */
};
Config sCfg = { false, 0, 125, true, false, 0, true, 0, 0 };

/* One RSP cache slot as the course's ops left it. `ver` changes on every VTX or MOD that
 * touches the slot; `posVer` only on VTX (the screen reads positions only). */
struct Slot {
    const Gdx3dsVtxIn* src;
    uint32_t pass;
    uint32_t ver;
    uint32_t posVer;
    uint32_t interpVer; /* == ver: R.vertices[slot] holds this vertex */
    uint32_t screenVer; /* == posVer: the screen below is current */
    uint32_t streamVer; /* == ver && streamRun == sRunSerial: streamIdx is current */
    uint32_t streamRun;
    uint16_t texS, texT;
    bool modified;
    uint32_t modVal;
    /* screen: the interpreter's fp32 clip position and flags, and the float24 error terms */
    float x, y, z, w;
    uint8_t clipRej, clipOut;
    bool rangeOk;
    int16_t rel[3];
    float errXY; /* 2^-14 row sums: max(e_x * xs, e_y) + e_w */
    float wr;    /* w from the rebased position (double compose) */
    double c[4]; /* clip coordinates of the rebased position (double; x times the aspect scale) */
    uint16_t streamIdx;
    uint32_t censusVer; /* track_opfold: == posVer when the census has this vertex (a mixed op's ring) */
};
Slot sSlot[kSlots];
uint32_t sVerCounter = 1;
uint32_t sRunSerial = 1;

/* The pass: one camera's course walk (constant MP). */
struct Pass {
    uint32_t serial;
    const GdxTrkRecord* rec;
    uint32_t frame;
    float mp[4][4];
    int32_t origin[3];
    float mpRel[4][4]; /* mp with row 3 translated to the origin (double compose) */
    float xs;
    float nearW;
    ClipXf clip; /* mpRel and xs in double: the transform the clipper plans against */
};
Pass sPass = {};

struct Window {
    uint32_t frames, lastFrame;
    int32_t course;
    uint32_t passes, runs, cpuRuns;
    uint32_t gpuDraws, gpuTris, cpuTris, dropTris;
    uint32_t fbFirst, fbClip, fbErr, fbRange, fbState, fbPool, fbSlot;
    uint32_t streamVerts, materialised;
    uint32_t nearTris, sideTris, nearDropped, fineVerts, fineDraws, fbNear;
    uint32_t maxErrMpx;    /* worst bound handed to the PICA */
    uint32_t maxErrAll;    /* worst bound of every candidate, refused or not */
    uint32_t hist[4];      /* candidates by bound: < 1/64 px, < 1/8, < 1, >= 1 */
    float nearWMin;
    uint64_t ticks;
    /* [native] track_resident (C5 Stage 3c) */
    uint32_t resOps, frameOps, resTris, resDraws;
    uint32_t missJoin, missSlot, missRing, missUv, missNear, missRej, missSide, missErr;
    uint32_t resMaxErr;
    uint32_t kind[16]; /* resolved rings by variant kind (gdx3ds_track_resident.h) */
    uint32_t verifyN, verifyBad;
    uint32_t exactChecks; /* rings the sphere could not clear, decided on their vertices */
    uint32_t probes, events; /* segment state resolutions through the interpreter; combiner events */
    /* [native] track_mattab (C5 Stage 3d) */
    uint32_t matQual, matServed, matDrop, matStock, matNoInterp;
    uint32_t matRes[GDXSEAM_MM_RESULT_COUNT];
    uint32_t matBanked, matInval, matBinds, matChecks, matMism, matMismBits, matLive, matGeoDrop;
    /* [native] track_opfold (C5 Stage 3d) */
    uint32_t mixOps, mixResTris, mixFrameTris, foldRuns, foldTris;
};
Window sWin = {};
uint32_t sMaxErrRun = 0; /* whole-session worst bound handed to the PICA */
uint32_t sWinKey = 0xFFFFFFFFu;
bool sInitLogged = false;

uint16_t sIdx[kMaxIdx];

/* ---- [trkcat]: where the retained replay's time goes (c5s3c-progress.md §1) ----------------
 *
 * Sub-buckets of the replay, ms per rendered frame, on the svcGetSystemTick clock. Armed only
 * with the [c5cat] clock ([debug] gputrace=1 or mainprof=1) AND [debug] trkcat=1: the clock is
 * read at every category change (several reads per triangle), so an armed run's `trk` is
 * inflated; the receipt prints the reads and their calibrated cost, and every bucket is printed
 * with its own reads' cost subtracted. Static counters only (no heap). */
enum TrkCat {
    TC_SETUP = 0, /* run prologue: config, pass, pool reservation, pack-state snapshot */
    TC_LATCH,     /* VTX / MOD ops: latching the recorded slots (the recorder's output) */
    TC_SCREEN,    /* the interpreter's fp32 position expressions + clip flags per vertex */
    TC_REBASE,    /* ob - E and the double clip coordinates of the rebased vertex */
    TC_GATE,      /* float24 error terms and the per-triangle bound (incl. visible refinement) */
    TC_STATE,     /* segment state: DerivePrep, BuildXf, combiner events */
    TC_PROBE,     /* the probe: the segment's first surviving triangle through the interpreter */
    TC_NEAR,      /* CPU near-plane (and side-plane) clipping + the fine stream */
    TC_STREAM,    /* stream vertex writes + index append */
    TC_SUBMIT,    /* GPU draw submission (backend xf draw, uniforms, state) */
    TC_CPU,       /* triangles drawn by the CPU path (fallbacks) */
    TC_TRI,       /* the rest of the per-triangle loop: slot checks, drop screen, bookkeeping */
    TC_RES,       /* [native] track_resident: ring lookup, variant match, pair check, indices */
    TC_COUNT
};
const char* const kTcName[TC_COUNT] = { "setup", "latch", "screen", "rebase", "f24",  "state", "probe",
                                        "near",  "stream", "submit", "cpu",   "tri", "res" };

struct TrkCatWin {
    uint64_t acc[TC_COUNT];
    uint32_t reads[TC_COUNT];
};
struct TrkCatState {
    bool on;   /* this run is being timed */
    int cfg;   /* [debug] trkcat, -1 = not read */
    int cur;
    uint64_t t;
    float readTicks; /* one clock read, calibrated when first armed */
    TrkCatWin win;
};
TrkCatState sTc = { false, -1, TC_SETUP, 0, 0.0f, {} };

/* Switch the open bucket; returns the previous one (-1 = not timing). */
inline int TcEnter(int cat) {
    if (!sTc.on) {
        return -1;
    }
    const int prev = sTc.cur;
    if (cat == prev) {
        return prev;
    }
    const uint64_t now = svcGetSystemTick();
    sTc.win.acc[prev] += now - sTc.t;
    sTc.win.reads[prev]++;
    sTc.t = now;
    sTc.cur = cat;
    return prev;
}

inline void TcLeave(int prev) {
    if (prev >= 0) {
        TcEnter(prev);
    }
}

void TcCalibrate() {
    enum { kReads = 256 };
    const uint64_t t0 = svcGetSystemTick();
    for (int i = 0; i < kReads; i++) {
        (void)svcGetSystemTick(); /* an SVC: never elided */
    }
    const uint64_t t1 = svcGetSystemTick();
    sTc.readTicks = (float)(t1 - t0) / (float)kReads;
}

bool TcArmed() {
    if (gdx3ds_c5cat_armed == nullptr || !gdx3ds_c5cat_armed()) {
        return false;
    }
    if (sTc.cfg < 0 && gdx3ds_config_get_int != nullptr) {
        sTc.cfg = gdx3ds_config_get_int("debug", "trkcat", 0) != 0 ? 1 : 0;
        if (sTc.cfg != 0) {
            TcCalibrate();
        }
    }
    return sTc.cfg > 0;
}

void TcBeginRun(uint64_t t0) {
    sTc.on = TcArmed();
    sTc.cur = TC_SETUP;
    sTc.t = t0;
}

void TcEndRun() {
    if (!sTc.on) {
        return;
    }
    const uint64_t now = svcGetSystemTick();
    sTc.win.acc[sTc.cur] += now - sTc.t;
    sTc.win.reads[sTc.cur]++;
    sTc.on = false;
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    if (gdx_dbg_logf == nullptr) {
        return;
    }
    char buf[1024];
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
    sCfg.level = gdx3ds_config_get_int("native", "track_retained", 0);
    if (sCfg.level < LEVEL_OFF || sCfg.level > LEVEL_PARITY) {
        sCfg.level = LEVEL_OFF;
    }
    sCfg.maxErr = gdx3ds_config_get_int("native", "track_retained_maxerr", 125);
    sCfg.rebase = gdx3ds_config_get_int("native", "track_retained_rebase", 1) != 0;
    sCfg.sideClipAlways = gdx3ds_config_get_int("native", "track_retained_sideclip", 0) != 0;
    sCfg.matTab = gdx3ds_config_get_int("native", "track_mattab", 0);
    if (sCfg.matTab < 0 || sCfg.matTab > 2 || sCfg.level != LEVEL_PERF) {
        sCfg.matTab = 0;
    }
    sCfg.matLive = gdx3ds_config_get_int("native", "track_mattab_live", 1) != 0;
    sCfg.nearClip = gdx3ds_config_get_int("native", "track_nearclip", 0);
    if (sCfg.nearClip < 0 || sCfg.nearClip > 1 || sCfg.level != LEVEL_PERF) {
        sCfg.nearClip = 0;
    }
    sCfg.opFold = gdx3ds_config_get_int("native", "track_opfold", 0);
    if (sCfg.opFold < 0 || sCfg.opFold > 1 || sCfg.level != LEVEL_PERF) {
        sCfg.opFold = 0;
    }
}

/* ---- the pass: origin, rebased matrix, near-plane w ------------------------------------ */

float XScaleOf(const Gdx3dsVtxMathState& vs) {
    if (vs.xMode == GDX3DS_VTX_X_STRETCH) {
        return 160.0f / (160.0f - 12.0f);
    }
    if (vs.xMode == GDX3DS_VTX_X_ADJ) {
        return (4.0f / 3.0f) / vs.aspect;
    }
    return 1.0f;
}

void BeginPass(const GdxTrkRecord* rec, const Gdx3dsVtxMathState& vs) {
    sPass.serial++;
    sPass.rec = rec;
    sPass.frame = rec->frame;
    memcpy(sPass.mp, vs.mp, sizeof(sPass.mp));
    double eye[3] = { 0.0, 0.0, 0.0 };
    const bool haveEye = SolveEye(vs.mp, eye);
    for (int k = 0; k < 3; k++) {
        const double e = (sCfg.rebase && haveEye) ? std::floor(eye[k] + 0.5) : 0.0;
        sPass.origin[k] = (e > 32767.0) ? 32767 : (e < -32768.0 ? -32768 : (int32_t)e);
    }
    memcpy(sPass.mpRel, vs.mp, sizeof(sPass.mpRel));
    for (int i = 0; i < 4; i++) {
        double t = vs.mp[3][i];
        for (int k = 0; k < 3; k++) {
            t += (double)sPass.origin[k] * (double)vs.mp[k][i];
        }
        sPass.mpRel[3][i] = (float)t;
    }
    sPass.xs = XScaleOf(vs);
    sPass.nearW = haveEye ? NearPlaneW(vs.mp, eye) : 1.0f;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 4; i++) {
            sPass.clip.m[k][i] = sPass.mpRel[k][i];
        }
    }
    sPass.clip.xs = sPass.xs;
    sWin.passes++;
    if (sWin.nearWMin == 0.0f || sPass.nearW < sWin.nearWMin) {
        sWin.nearWMin = sPass.nearW;
    }
}

bool SamePass(const GdxTrkRecord* rec, const Gdx3dsVtxMathState& vs) {
    return sPass.serial != 0 && sPass.rec == rec && sPass.frame == rec->frame &&
           memcmp(sPass.mp, vs.mp, sizeof(sPass.mp)) == 0;
}

/* ---- slots ------------------------------------------------------------------------------ */

void LoadSlot(GdxInterpSeam& I, int s, const Gdx3dsVtxIn* src) {
    Slot& sl = sSlot[s];
    sl.src = src;
    sl.pass = sPass.serial;
    sl.ver = ++sVerCounter;
    sl.posVer = sl.ver;
    sl.texS = *I.R().tex_scale_s;
    sl.texT = *I.R().tex_scale_t;
    sl.modified = false;
}

void ModifySlot(int s, uint32_t val) {
    Slot& sl = sSlot[s];
    sl.ver = ++sVerCounter;
    sl.modified = true;
    sl.modVal = val;
}

/* The interpreter's short U/V for a slot: the load-time scale (GfxSpVertex), or the ST patch
 * (SpModifyVertex writes the shorts verbatim). */
void SlotUv(const Slot& sl, int16_t* u, int16_t* v) {
    if (sl.modified) {
        *u = (int16_t)(sl.modVal >> 16);
        *v = (int16_t)sl.modVal;
        return;
    }
    *u = (int16_t)(((int)sl.src->tc[0] * (int)sl.texS) >> 16);
    *v = (int16_t)(((int)sl.src->tc[1] * (int)sl.texT) >> 16);
}

/* Make R.vertices[s] exactly what the CPU replay's SpVertex (+ ModifyVertex) left there. */
void Materialise(GdxInterpSeam& I, int s) {
    Slot& sl = sSlot[s];
    if (sl.interpVer == sl.ver) {
        return;
    }
    I.SpVertex(1, (size_t)s, sl.src);
    GdxSeamVertex& d = I.R().vertices[s];
    if (sl.modified || sl.texS != *I.R().tex_scale_s || sl.texT != *I.R().tex_scale_t) {
        int16_t u;
        int16_t v;
        SlotUv(sl, &u, &v);
        d.u = u;
        d.v = v;
    }
    sl.interpVer = sl.ver;
    sWin.materialised++;
}

/* The position half of GfxSpVertex (same expressions, same order: the flags are the CPU
 * path's), plus the rebased shorts and the float24 error terms of the rebased dp4. */
void Screen(const Gdx3dsVtxMathState& vs, Slot& sl) {
    if (sl.screenVer == sl.posVer) {
        return;
    }
    const int tcPrev = TcEnter(TC_SCREEN);
    const int16_t* ob = sl.src->ob;
    const float(*mp)[4] = vs.mp;
    float x = ob[0] * mp[0][0] + ob[1] * mp[1][0] + ob[2] * mp[2][0] + mp[3][0];
    const float y = ob[0] * mp[0][1] + ob[1] * mp[1][1] + ob[2] * mp[2][1] + mp[3][1];
    const float z = ob[0] * mp[0][2] + ob[1] * mp[1][2] + ob[2] * mp[2][2] + mp[3][2];
    const float w = ob[0] * mp[0][3] + ob[1] * mp[1][3] + ob[2] * mp[2][3] + mp[3][3];
    if (vs.xMode == GDX3DS_VTX_X_STRETCH) {
        x = x * (160.0f / (160.0f - 12.0f));
    } else if (vs.xMode == GDX3DS_VTX_X_ADJ) {
        x = x * (4.0f / 3.0f) / vs.aspect;
    }
    Gdx3dsLoadedVtx t;
    t.x = x;
    t.y = y;
    t.z = z;
    t.w = w;
    sl.clipRej = 0;
    if (x < -w) sl.clipRej |= 1;
    if (x > w) sl.clipRej |= 2;
    if (y < -w) sl.clipRej |= 4;
    if (y > w) sl.clipRej |= 8;
    if (z > w) sl.clipRej |= 32;
    sl.clipOut = gdx3ds_vtx_clip_out_mask(&t);
    sl.x = x;
    sl.y = y;
    sl.z = z;
    sl.w = w;
    TcEnter(TC_REBASE);
    sl.rangeOk = true;
    float a[3];
    for (int k = 0; k < 3; k++) {
        const int32_t r = (int32_t)ob[k] - sPass.origin[k];
        if (r < -32768 || r > 32767) {
            sl.rangeOk = false;
        }
        sl.rel[k] = (int16_t)r;
        a[k] = std::fabs((float)r);
    }
    TcEnter(TC_GATE);
    const float(*m)[4] = sPass.mpRel;
    float e[4];
    for (int i = 0; i < 4; i++) {
        e[i] = (std::fabs(m[0][i]) * a[0] + std::fabs(m[1][i]) * a[1] + std::fabs(m[2][i]) * a[2] +
                std::fabs(m[3][i])) *
               kF24Row;
    }
    const float ex = e[0] * sPass.xs;
    sl.errXY = (ex > e[1] ? ex : e[1]) + e[3];
    TcEnter(TC_REBASE);
    const double pr[3] = { (double)sl.rel[0], (double)sl.rel[1], (double)sl.rel[2] };
    GdxTrkRr::ClipOf(sPass.clip, pr, sl.c);
    sl.wr = (float)sl.c[3];
    sl.screenVer = sl.posVer;
    TcLeave(tcPrev);
}

/* ---- the GPU segment ---------------------------------------------------------------------- */

struct GpuSeg {
    GdxInterpSeam* I;
    Gdx3dsVtxIn* stream; /* this run's reservation in the frame's linear pool */
    uint32_t streamCap;
    uint32_t nStream;
    uint32_t nIdx;
    float clsW;
    bool xfValid;        /* uniforms below match the resolved state */
    Gdx3dsXfDraw xf;
    Gdx3dsVboPackAux aux;
    bool useFog, useBlendColor;
    bool sideClip; /* clip at the side planes too (latched with the state) */
};

float ByteUnit(uint8_t b) {
    return b / 255.0f;
}

bool InputIsVertexDependent(uint8_t mux) {
    return mux == GDXSEAM_CCMUX_SHADE_ALPHA || mux == GDXSEAM_CCMUX_LOD_FRACTION;
}

/* The gpuxf uniforms for the state the exact path resolved: XfFillMesh/XfFillRun's mapping,
 * from the fast path's prep (the parameters the CPU pack would have used). false = a combiner
 * input the per-draw uniforms cannot carry (the CPU path keeps the segment). */
bool BuildXf(GpuSeg& g, const RunCtx& c, const Gdx3dsVtxMathState& vs) {
    const GdxTrackPackState& ps = c.ps;
    const GdxSeamRegs& R = g.I->R();
    const Gdx3dsVboPackParams& pp = *ps.params;
    const uint8_t(*inputMap)[7] = ps.inputMap;
    if (ps.aux == nullptr || inputMap == nullptr) {
        return false;
    }
    if (c.ci >= 0 && !c.colorIsShade && InputIsVertexDependent(inputMap[0][c.ci])) {
        return false;
    }
    if (pp.wantVtxAlpha && !pp.fogFactorToAlpha && c.ai >= 0 && !c.alphaIsShade &&
        InputIsVertexDependent(inputMap[1][c.ai])) {
        return false;
    }
    Gdx3dsXfDraw& xf = g.xf;
    memset(&xf, 0, sizeof(xf));
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            xf.mvp[i][k] = sPass.mpRel[k][i];
        }
    }
    xf.conv[0] = sPass.xs;
    xf.conv[1] = ps.clip.invertY ? -1.0f : 1.0f;
    xf.conv[2] = ps.clip.zIsFrom0To1 ? 0.5f : 1.0f;
    xf.conv[3] = 1.0f;
    xf.convw[2] = ps.clip.zIsFrom0To1 ? 0.5f : 0.0f;
    const float fogOn = vs.fog ? 1.0f : 0.0f;
    xf.fog[0] = (float)vs.fogMul;
    xf.fog[1] = (float)vs.fogOffset;
    xf.fog[2] = fogOn;
    xf.fog[3] = 1.0f - fogOn;
    xf.unlit[3] = 1.0f; /* lit = 0: shade = the vertex colour */
    xf.tg[3] = 1.0f;    /* no texgen */
    xf.tcs[0] = 1.0f;   /* the stream carries the final short U/V: U = floor(tc * 1) */
    xf.tcs[1] = 1.0f;
    for (int t = 0; t < 2; t++) {
        xf.uvc[t][0] = 1.0e18f;
        xf.uvc[t][1] = 1.0e18f;
        if (!ps.usedTextures[t]) {
            continue;
        }
        const float nu = c.prep[t].uMul ? c.prep[t].uNormMul : 1.0f / c.prep[t].uNormDiv;
        const float nv = c.prep[t].vMul ? c.prep[t].vNormMul : 1.0f / c.prep[t].vNormDiv;
        const float half = c.halfTexel ? 0.5f : 0.0f;
        xf.uva[t][0] = c.prep[t].uPre / 32.0f * nu * pp.uScale[t];
        xf.uva[t][1] = c.prep[t].vPre / 32.0f * nv * pp.vScale[t];
        xf.uva[t][2] = (half - c.prep[t].uOff) * nu * pp.uScale[t];
        xf.uva[t][3] = (half - c.prep[t].vOff) * nv * pp.vScale[t];
        xf.uvc[t][0] = c.clampU[t] < 1.0e18f ? c.clampU[t] : 1.0e18f;
        xf.uvc[t][1] = c.clampV[t] < 1.0e18f ? c.clampV[t] : 1.0e18f;
        xf.uvc[t][2] = pp.hasOff ? pp.uOff[t] : 0.0f;
        xf.uvc[t][3] = pp.hasOff ? pp.vOff[t] : 0.0f;
    }
    /* colour: FastTri's out[6..8] */
    float selC = 0.0f;
    if (c.ci >= 0 && c.colorIsShade) {
        selC = 1.0f;
    } else if (c.ci >= 0) {
        const GdxNativeTri::InputResult cv = GdxNativeTri::EvalInput(R, inputMap, c.ci, 0, &R.vertices[0], &R.vertices[0]);
        xf.col[0] = ByteUnit(cv.c.r);
        xf.col[1] = ByteUnit(cv.c.g);
        xf.col[2] = ByteUnit(cv.c.b);
    } else {
        xf.col[0] = xf.col[1] = xf.col[2] = 1.0f;
    }
    /* alpha: FastTri's out[9] (the vertex alpha is the fog factor under G_FOG, as in the shader) */
    float selA = 0.0f;
    xf.col[3] = 1.0f;
    if (pp.fogFactorToAlpha) {
        if (c.fogFactorConst) {
            xf.col[3] = c.fogFactorConstVal;
        } else {
            selA = 1.0f;
        }
    } else if (pp.wantVtxAlpha) {
        if (c.alphaIsShade) {
            selA = c.shadeAlphaIsOne ? 0.0f : 1.0f;
        } else {
            const GdxNativeTri::InputResult av =
                GdxNativeTri::EvalInput(R, inputMap, c.ai, 1, &R.vertices[0], &R.vertices[0]);
            xf.col[3] = ByteUnit(av.c.a);
        }
    }
    xf.sel[0] = selC;
    xf.sel[1] = selA;
    xf.seli[0] = 1.0f - selC;
    xf.seli[1] = 1.0f - selA;
    /* cull: FastTri's cross-product rule, G_EX_INVERT_CULLING folded in (XfRun's mapping) */
    const uint32_t cullType = *R.geometry_mode & ps.cullBoth;
    int cull = GDX3DS_CULL_NONE;
    if (cullType != 0 && cullType == ps.cullBoth) {
        cull = GDX3DS_CULL_BOTH;
    } else if (cullType != 0 && cullType == ps.cullFront) {
        cull = GDX3DS_CULL_FRONT;
    } else if (cullType != 0 && cullType == ps.cullBack) {
        cull = GDX3DS_CULL_BACK;
    }
    if ((*R.extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0) {
        if (cull == GDX3DS_CULL_FRONT) {
            cull = GDX3DS_CULL_BACK;
        } else if (cull == GDX3DS_CULL_BACK) {
            cull = GDX3DS_CULL_FRONT;
        }
    }
    if (cull == GDX3DS_CULL_BOTH) {
        return false; /* every tri culls: the CPU path drops them all, cheaply */
    }
    xf.cullMode = cull;
    memcpy(&g.aux, ps.aux, sizeof(g.aux));
    /* the state (viewport included) is resolved by now: is the viewport folded? */
    g.sideClip = sCfg.sideClipAlways || gdx3ds_track_backend_viewport_folded() != 0;
    g.useFog = c.useFog;
    g.useBlendColor = c.useBlendColor;
    return true;
}

void FlushGpu(GpuSeg& g) {
    if (g.nIdx == 0) {
        return;
    }
    const int tcPrev = TcEnter(TC_SUBMIT);
    g.xf.verts = g.stream;
    g.xf.nVerts = g.nStream;
    g.xf.clsW = g.clsW;
    g.I->ApplyDrawConstants();
    if (g.useFog) {
        if (g.useBlendColor) {
            gdx3ds_fog_note_params(0, 0, 0);
        } else {
            gdx3ds_fog_note_params(*g.I->R().fog_mul, *g.I->R().fog_offset, 1);
        }
    }
    gdx3ds_mesh_backend_xf_draw(sIdx, g.nIdx, &g.aux, &g.xf);
    sWin.gpuDraws++;
    g.nIdx = 0;
    TcLeave(tcPrev);
}

void CensusNote(const Slot& sl) {
    const float(*m)[4] = sPass.mpRel;
    const int16_t* r = sl.rel;
    /* the reference is the exact (double) value of the dp4 the shader runs */
    const double x = ((double)r[0] * m[0][0] + (double)r[1] * m[1][0] + (double)r[2] * m[2][0] + m[3][0]) * sPass.xs;
    const double y = (double)r[0] * m[0][1] + (double)r[1] * m[1][1] + (double)r[2] * m[2][1] + m[3][1];
    const double w = (double)r[0] * m[0][3] + (double)r[1] * m[1][3] + (double)r[2] * m[2][3] + m[3][3];
    gdx3ds_xf_census_path = GDX3DS_XFC_TRK;
    gdx3ds_xf_census_note(r, m, sPass.xs, (float)x, (float)y, (float)w);
    gdx3ds_xf_census_path = GDX3DS_XFC_DL;
}

/* The slot's index in this run's stream (appending the rebased vertex on first use). */
bool StreamIndex(GpuSeg& g, int s, uint16_t* out) {
    Slot& sl = sSlot[s];
    if (sl.streamVer == sl.ver && sl.streamRun == sRunSerial) {
        *out = sl.streamIdx;
        return true;
    }
    if (g.nStream >= g.streamCap) {
        return false;
    }
    const int tcPrev = TcEnter(TC_STREAM);
    Gdx3dsVtxIn& d = g.stream[g.nStream];
    d.ob[0] = sl.rel[0];
    d.ob[1] = sl.rel[1];
    d.ob[2] = sl.rel[2];
    d.flag = 0;
    SlotUv(sl, &d.tc[0], &d.tc[1]);
    memcpy(d.cn, sl.src->cn, 4);
    sl.streamIdx = (uint16_t)g.nStream;
    sl.streamVer = sl.ver;
    sl.streamRun = sRunSerial;
    g.nStream++;
    sWin.streamVerts++;
    if (gdx3ds_xf_census_on) {
        CensusNote(sl);
    }
    *out = sl.streamIdx;
    TcLeave(tcPrev);
    return true;
}

/* ---- the run --------------------------------------------------------------------------- */

/* The near-plane polygons' draw: its own stream chunk (reserved lazily, at a point where no
 * batch is open) and the coarse uniforms with the position rows scaled by 1/kFine. */
struct FineSeg {
    FineVtx* stream;
    uint32_t cap;
    uint32_t n;
    uint32_t nIdx;
    float clsW;
    bool xfValid;
    Gdx3dsXfDraw xf;
    uint16_t idx[kMaxFineIdx];
};

struct Run {
    GdxInterpSeam* I;
    Gdx3dsVtxMathState vs;
    RunCtx c;
    GpuSeg g;
    FineSeg f;
    bool forceCpu;
    const GdxTrkResView* res; /* [native] track_resident: the adopted bake, NULL = per-frame only */
};

/* ---- [native] track_resident: the resident draw (C5 Stage 3c) ------------------------------
 *
 * Indices into the course's resident buffer (the adopted bake, world-space rings), drawn with
 * the pass's own uniforms plus `xoff` = -origin: the shader's exact integer add gives the dp4
 * the same eye-rebased vertex the stream below would carry. One open resident draw at a time;
 * painter's order with the stream, fine and CPU draws is kept by flushing the others first. */
uint16_t sResIdx[kMaxIdx];
uint32_t sResN = 0;
float sResClsW = 1.5f;

void FlushRes(Run& r) {
    if (sResN == 0) {
        return;
    }
    const int tcPrev = TcEnter(TC_SUBMIT);
    GpuSeg& g = r.g;
    Gdx3dsXfDraw xf = g.xf; /* the segment's uniforms (they never change with indices pending) */
    xf.verts = r.res->linear;
    xf.nVerts = r.res->nVerts;
    xf.clsW = sResClsW;
    for (int k = 0; k < 3; k++) {
        xf.off[k] = -(float)sPass.origin[k];
    }
    xf.off[3] = 0.0f;
    g.I->ApplyDrawConstants();
    if (g.useFog) {
        if (g.useBlendColor) {
            gdx3ds_fog_note_params(0, 0, 0);
        } else {
            gdx3ds_fog_note_params(*g.I->R().fog_mul, *g.I->R().fog_offset, 1);
        }
    }
    gdx3ds_mesh_backend_xf_draw(sResIdx, sResN, &g.aux, &xf);
    sWin.gpuDraws++;
    sWin.resDraws++;
    sResN = 0;
    TcLeave(tcPrev);
}

void FlushFine(Run& r) {
    FineSeg& f = r.f;
    if (f.nIdx == 0) {
        return;
    }
    const int tcPrev = TcEnter(TC_SUBMIT);
    f.xf.verts = f.stream;
    f.xf.nVerts = f.n;
    f.xf.clsW = f.clsW;
    GpuSeg& g = r.g;
    g.I->ApplyDrawConstants();
    if (g.useFog) {
        if (g.useBlendColor) {
            gdx3ds_fog_note_params(0, 0, 0);
        } else {
            gdx3ds_fog_note_params(*g.I->R().fog_mul, *g.I->R().fog_offset, 1);
        }
    }
    gdx3ds_track_backend_xf_draw_fpos(f.idx, f.nIdx, &g.aux, &f.xf);
    sWin.gpuDraws++;
    sWin.fineDraws++;
    f.nIdx = 0;
    /* the chunk's vertices stay referenced by the submitted draw; the next polygon starts a
       fresh chunk (indices are chunk-relative) */
    f.stream = nullptr;
    f.n = 0;
    f.cap = 0;
    TcLeave(tcPrev);
}

void FlushAllGpu(Run& r) {
    FlushRes(r);
    FlushGpu(r.g);
    FlushFine(r);
}

bool SlotLive(int s) {
    return s < kSlots && sSlot[s].pass == sPass.serial && sSlot[s].src != nullptr;
}

/* A slot this pass never loaded is left as the interpreter holds it (the stock replay would read
 * the same stale cache entry). */
void MaterialiseIfLive(GdxInterpSeam& I, int s) {
    if (SlotLive(s)) {
        Materialise(I, s);
    }
}

void CpuTri(Run& r, uint8_t a, uint8_t b, uint8_t d) {
    const int tcPrev = TcEnter(TC_CPU);
    FlushAllGpu(r);
    MaterialiseIfLive(*r.I, a);
    MaterialiseIfLive(*r.I, b);
    MaterialiseIfLive(*r.I, d);
    GdxNativeTri::Tri(r.c, a, b, d);
    sWin.cpuTris++;
    TcLeave(tcPrev);
}

/* F3DEX2.Rej's box (FastTri / gdx3ds_tri_rej_reject). */
bool RejBox(const Slot& v) {
    const float w2 = 2.0f * v.w;
    return v.w <= 0.0f || v.x < -w2 || v.x > w2 || v.y < -w2 || v.y > w2 || v.z < -v.w;
}

/* The float24 bound of a triangle that does not reach the near plane, 1/1000 px. Every point
 * the PICA rasterises is a perspective blend of the three vertices, and a blend's error ratio
 * never exceeds the worst vertex's (mediant inequality): max over the vertices of e / w. */
uint32_t BoundMpx(float e, float w) {
    if (!(w > 0.0f)) {
        return 4000000000u;
    }
    const float px = e / w * 200.0f * 1000.0f;
    return px > 4.0e9f ? 4000000000u : (uint32_t)px;
}

uint32_t TriBoundMpx(const Slot& v1, const Slot& v2, const Slot& v3) {
    uint32_t b = BoundMpx(v1.errXY, v1.wr);
    const uint32_t b2 = BoundMpx(v2.errXY, v2.wr);
    const uint32_t b3 = BoundMpx(v3.errXY, v3.wr);
    b = b2 > b ? b2 : b;
    return b3 > b ? b3 : b;
}

void NoteBound(uint32_t bound) {
    if (bound > sWin.maxErrAll) {
        sWin.maxErrAll = bound;
    }
    if (bound < 16u) {
        sWin.hist[0]++;
    } else if (bound < 125u) {
        sWin.hist[1]++;
    } else if (bound < 1000u) {
        sWin.hist[2]++;
    } else {
        sWin.hist[3]++;
    }
}

enum ProbeResult { PROBE_RESOLVED, PROBE_DROPPED, PROBE_DRAWN };
ProbeResult ProbeStock(Run& r, uint8_t a, uint8_t b, uint8_t d);
uint8_t FogByte(const Gdx3dsVtxMathState& vs, const Slot& sl);

/* ---- [native] track_mattab: the probe from a course material table (C5 Stage 3d) --------------
 *
 * The probe below runs the interpreter's whole triangle path on a segment's first surviving
 * triangle to resolve the segment's draw state, then forgets what it appended. For a triangle
 * with no vertex outside any clip plane (SpTri's clipper cannot run) that already passed PreDrop
 * (the reject box, the trivial reject and the backface rule: the drops SpTri makes before its
 * state block), with nothing pending, SpTri is certain to reach its state block, and what it does
 * there is a function of the registers plus vertex 0's colour and w only
 * (gdx_tri.cpp TrackMatOps). The table records the state block per register key -- the
 * machines' MachMatRec, with the same invalidation proof (key compare, texture-cache slot and
 * key, classification epoch, sampler) -- and a hit replays its apply half; vertex 0 comes from
 * the slot the probe would have materialised (ProbeVertex). Verify mode (2) always probes and
 * compares. The table is course-scoped: emptied when the course changes. */
constexpr int kMatBuckets = 16;
constexpr int kMatWays = 4;
GdxSeamMatRec sMat[kMatBuckets * kMatWays];
int32_t sMatCourse = -0x7fffffff;
bool sMatInitLogged = false;

int MatMode() {
    return sCfg.matTab | (sCfg.matLive ? GDXSEAM_MATREC_LIVE : 0);
}

void MatCourse(int32_t course) {
    if (course != sMatCourse) {
        sMatCourse = course;
        memset(sMat, 0, sizeof(sMat));
    }
}

/* R.vertices[s] as the probe would read it after MaterialiseIfLive: the interpreter's copy when it
 * is current, else GfxSpVertex's outputs for the slot under this run's latched state -- the
 * position and clip flags are Screen's (the same expressions), the colour is the Vtx colour with
 * lighting off (a lit run never gets here), its alpha the fog byte under G_FOG. */
GdxSeamVertex ProbeVertex(const Run& r, int s) {
    const Slot& sl = sSlot[s];
    if (sl.interpVer == sl.ver) {
        return r.I->R().vertices[s];
    }
    GdxSeamVertex v;
    memset(&v, 0, sizeof(v));
    v.x = sl.x;
    v.y = sl.y;
    v.z = sl.z;
    v.w = sl.w;
    int16_t u;
    int16_t t;
    SlotUv(sl, &u, &t);
    v.u = u;
    v.v = t;
    v.color.r = sl.src->cn[0];
    v.color.g = sl.src->cn[1];
    v.color.b = sl.src->cn[2];
    v.color.a = r.vs.fog ? FogByte(r.vs, sl) : sl.src->cn[3];
    v.clip_rej = sl.clipRej;
    v.clip_out = sl.clipOut;
    return v;
}

/* The probe may be served: nothing pending (PreDrop, the reject screen included, already passed;
 * the interpreter replays the rest of SpTri's geometry itself). */
bool MatServable(const Run& r) {
    return r.I->PendingTris() == 0;
}

void NoteMatBefore(int rc, bool serve, const GdxSeamMatOutcome& o) {
    if (rc < 0) {
        sWin.matNoInterp++;
        return;
    }
    sWin.matQual += serve ? 1u : 0u;
    if (rc != 4 && o.result < GDXSEAM_MM_RESULT_COUNT) {
        sWin.matRes[o.result]++;
    }
    if (rc == 1) {
        sWin.matServed++;
        sWin.matBinds += o.binds;
        sWin.matLive += o.live != 0 ? 1u : 0u;
    } else if (rc == 2) {
        sWin.matDrop++;
    } else if (rc == 3) {
        sWin.matStock++;
    } else if (rc == 4) {
        sWin.matGeoDrop++;
    }
}

void NoteMatAfter(const GdxSeamMatOutcome& o) {
    sWin.matBanked += o.banked;
    sWin.matInval += o.wasValid;
    sWin.matChecks += o.checked;
    sWin.matMism += o.mismatch != 0 ? 1u : 0u;
    sWin.matMismBits |= o.mismatch;
}

/* Resolve the segment's draw state WITHOUT drawing its first triangle on the CPU: the
 * interpreter's triangle path runs on it (state block, texture import, sampler, aux latch --
 * every side effect the CPU path has), and what it appended to its packed batch is forgotten
 * (the interpreter re-begins its batch at the pool cursor on its next append). The triangle
 * itself then goes to the GPU with the rest, so a segment has no CPU/GPU seam at its start. */
ProbeResult ProbeState(Run& r, uint8_t a, uint8_t b, uint8_t d) {
    GdxInterpSeam& I = *r.I;
    sWin.probes++;
    RunCtx& c = r.c;
    FlushAllGpu(r);
    int mat = -1;
    if (sCfg.matTab != 0) {
        const bool serve = MatServable(r);
        /* before the probe materialises them: verify then checks these copies */
        const GdxSeamVertex tri[3] = { ProbeVertex(r, a), ProbeVertex(r, b), ProbeVertex(r, d) };
        GdxSeamMatOutcome mo;
        memset(&mo, 0, sizeof(mo));
        mat = I.TrackMatBefore(sMat, kMatBuckets, kMatWays, MatMode(), serve, tri, mo);
        NoteMatBefore(mat, serve, mo);
        if (mat == 1) {
            c.needExact = false;
            c.prepValid = false;
            r.g.xfValid = false;
            r.f.xfValid = false;
            return PROBE_RESOLVED;
        }
        if (mat == 2 || mat == 4) {
            return PROBE_DROPPED; /* a refused batch (the state is bound), or no triangle reaches the state */
        }
    }
    const ProbeResult pr = ProbeStock(r, a, b, d);
    if (mat == 0) {
        GdxSeamMatOutcome ao;
        memset(&ao, 0, sizeof(ao));
        const int probe = pr == PROBE_RESOLVED ? 0 : (pr == PROBE_DROPPED ? 1 : 2);
        I.TrackMatAfter(sMat, kMatBuckets, kMatWays, MatMode(), probe, ao);
        NoteMatAfter(ao);
    }
    return pr;
}

/* The stock probe: the interpreter's own triangle path on the segment's first surviving triangle. */
ProbeResult ProbeStock(Run& r, uint8_t a, uint8_t b, uint8_t d) {
    GdxInterpSeam& I = *r.I;
    RunCtx& c = r.c;
    MaterialiseIfLive(I, a);
    MaterialiseIfLive(I, b);
    MaterialiseIfLive(I, d);
    const size_t n0 = I.PendingTris();
    I.SpTri(a, b, d);
    const size_t n1 = I.PendingTris();
    if (n1 < n0) {
        c.stats_fallback++; /* a full-buffer flush inside: the interpreter drew it */
        return PROBE_DRAWN;
    }
    const size_t appended = n1 - n0;
    if (appended == 0) {
        return PROBE_DROPPED; /* rejected / culled before the state block */
    }
    I.PackState(&c.ps);
    if (!c.ps.packBatch || n0 != 0) {
        I.Flush(); /* keep painter's order: the interpreter's own batch draws it now */
        c.stats_fallback += (uint32_t)appended;
        return PROBE_DRAWN;
    }
    I.PopAppended(n0, appended);
    c.needExact = false;
    c.prepValid = false;
    r.g.xfValid = false;
    r.f.xfValid = false;
    return PROBE_RESOLVED;
}

/* FastTri's backface rule, expression for expression (clip_out == 0 triangles only). */
bool BackfaceCulled(const GdxSeamRegs& R, const GdxTrackPackState& ps, const Slot& v1, const Slot& v2,
                    const Slot& v3) {
    const uint32_t geometry_mode = *R.geometry_mode;
    if ((geometry_mode & ps.cullBoth) == 0) {
        return false;
    }
    const float dx1 = v1.x / (v1.w) - v2.x / (v2.w);
    const float dy1 = v1.y / (v1.w) - v2.y / (v2.w);
    const float dx2 = v3.x / (v3.w) - v2.x / (v2.w);
    const float dy2 = v3.y / (v3.w) - v2.y / (v2.w);
    float cross = dx1 * dy2 - dy1 * dx2;
    if ((v1.w < 0) ^ (v2.w < 0) ^ (v3.w < 0)) {
        cross = -cross;
    }
    if ((*R.extra_geometry_mode & GDXSEAM_G_EX_INVERT_CULLING) != 0) {
        cross = -cross;
    }
    const uint32_t cullType = geometry_mode & ps.cullBoth;
    if (cullType == ps.cullFront) {
        return cross <= 0;
    }
    if (cullType == ps.cullBack) {
        return cross >= 0;
    }
    return cullType == ps.cullBoth;
}

/* The drops GfxSpTri1 makes BEFORE it resolves any state (so dropping here is the CPU path's
 * outcome, side effects included): F3DEX2.Rej's box, all vertices outside one plane, and --
 * only while no state is resolved yet, where the alternative is a full exact-path round trip --
 * the backface rule for a triangle the clipper does not touch. */
bool PreDrop(Run& r, const Slot& v1, const Slot& v2, const Slot& v3) {
    const GdxTrackPackState& ps = r.c.ps;
    if (ps.rejectVariant && (RejBox(v1) || RejBox(v2) || RejBox(v3))) {
        return true;
    }
    if ((v1.clipRej & v2.clipRej & v3.clipRej) != 0) {
        return true;
    }
    if (r.c.needExact && (v1.clipOut | v2.clipOut | v3.clipOut) == 0) {
        return BackfaceCulled(r.I->R(), ps, v1, v2, v3);
    }
    return false;
}

BoundV BoundOf(const Slot& sl) {
    return { sl.x, sl.y, sl.w, sl.errXY };
}

/* ---- near-plane polygons ([native] track_retained=1) ---------------------------------------- */

/* The RSP fog byte of a corner, from the interpreter's own fp32 z and w (GfxSpVertex). */
uint8_t FogByte(const Gdx3dsVtxMathState& vs, const Slot& sl) {
    float fogRatio;
    if (sl.w <= 0.001f) {
        fogRatio = -1.0f;
    } else {
        fogRatio = gdx3ds_clampf(sl.z / sl.w, -1.0f, 1.0f);
    }
    const float fogZ = gdx3ds_clampf(fogRatio * vs.fogMul + vs.fogOffset, 0.0f, 255.0f);
    return (uint8_t)fogZ;
}

/* A corner as the CPU clipper would see it: its vertex colour and, under G_FOG, the fog byte in
 * the alpha (the clipper interpolates both; the fine draw carries them instead of letting the
 * shader recompute fog at a clip point, which is not linear along the edge). */
PolyV PolyOf(const Gdx3dsVtxMathState& vs, const Slot& sl) {
    PolyV o;
    for (int k = 0; k < 3; k++) {
        o.p[k] = sl.rel[k];
        o.rel[k] = sl.rel[k];
    }
    for (int i = 0; i < 4; i++) {
        o.c[i] = sl.c[i];
    }
    int16_t u;
    int16_t v;
    SlotUv(sl, &u, &v);
    o.u = u;
    o.v = v;
    for (int k = 0; k < 3; k++) {
        o.cn[k] = sl.src->cn[k];
    }
    o.cn[3] = vs.fog ? FogByte(vs, sl) : sl.src->cn[3];
    o.orig = true;
    return o;
}

int16_t Clamp16(double v) {
    return (int16_t)(v > 32767.0 ? 32767 : (v < -32768.0 ? -32768 : v));
}

/* The position half of a fine-stream vertex: its float position and the float24 bound term
 * e / w with the corner the visible-region refinement needs. A function of the position only. */
struct FinePos {
    float p[3];
    float eow;
    BoundV bv;
};

void FinePosOf(const PolyV& pv, FinePos* o) {
    double pf[3];
    for (int k = 0; k < 3; k++) {
        o->p[k] = pv.orig ? (float)pv.rel[k] : (float)pv.p[k];
        pf[k] = o->p[k];
    }
    const float(*m)[4] = sPass.mpRel;
    double e[4];
    double w = m[3][3];
    for (int i = 0; i < 4; i++) {
        e[i] = std::fabs(m[3][i]);
        for (int k = 0; k < 3; k++) {
            e[i] += std::fabs((double)m[k][i]) * std::fabs(pf[k]);
        }
        e[i] *= kF24Row;
    }
    for (int k = 0; k < 3; k++) {
        w += pf[k] * (double)m[k][3];
    }
    const double ex = e[0] * sPass.xs;
    const double exy = (ex > e[1] ? ex : e[1]) + e[3];
    o->eow = w > 0.0 ? (float)(exy / w) : 1.0e9f;
    double x = m[3][0];
    double y = m[3][1];
    for (int k = 0; k < 3; k++) {
        x += pf[k] * (double)m[k][0];
        y += pf[k] * (double)m[k][1];
    }
    o->bv.x = x * sPass.xs;
    o->bv.y = y;
    o->bv.w = w;
    o->bv.e = exy;
}

/* The attribute half: UV x kFineUv and the colour, clamped and truncated like gdx3ds_lerp_byte. */
void FineAttrOf(const PolyV& pv, FineVtx* d) {
    d->tc[0] = (float)(pv.u * kFineUv);
    d->tc[1] = (float)(pv.v * kFineUv);
    for (int k = 0; k < 4; k++) {
        const double c = pv.cn[k];
        d->cn[k] = (uint8_t)(c < 0.0 ? 0.0 : (c > 255.0 ? 255.0 : c));
    }
}

/* The fine-stream vertex of a polygon corner and its float24 bound term e / w. */
void FineVertex(const PolyV& pv, FineVtx* d, float* errOverW, BoundV* bv) {
    FinePos fp;
    FinePosOf(pv, &fp);
    memcpy(d->p, fp.p, sizeof(d->p));
    FineAttrOf(pv, d);
    *errOverW = fp.eow;
    *bv = fp.bv;
}

/* ---- [native] track_nearclip=1: the lean clipper's caches (C5 Stage 3d) --------------------
 *
 * GdxTrkRr::LeanClipPoly (the math header: ClipPoly's result bit for bit, pointers instead of
 * copies, one plane distance per corner, shared edge points cached) plus the position half of the
 * fine vertex, cached per original corner (per slot: its position version and the pass) and per
 * cached edge point (per entry stamp). Both halves are functions of the position alone, so a
 * cached value IS the recomputed one. */
struct SlotFine {
    uint32_t pass;
    uint32_t posVer;
    FinePos fp;
};
SlotFine sSlotFine[kSlots];
GdxTrkRr::EdgeCache sEdge = {};
struct EdgeFine {
    uint32_t stamp;
    FinePos fp;
};
EdgeFine sEdgeFine[GdxTrkRr::EdgeCache::kN];
uint32_t sLeanPosHit = 0;
uint32_t sLeanPosMiss = 0;

const FinePos& CornerPos(const PolyV& pv, int slot, int entry, uint32_t stamp, FinePos* scratch) {
    if (slot >= 0) {
        SlotFine& sf = sSlotFine[slot];
        if (sf.pass == sPass.serial && sf.posVer == sSlot[slot].posVer) {
            sLeanPosHit++;
            return sf.fp;
        }
        FinePosOf(pv, &sf.fp);
        sf.pass = sPass.serial;
        sf.posVer = sSlot[slot].posVer;
        sLeanPosMiss++;
        return sf.fp;
    }
    if (entry >= 0 && stamp != 0) {
        EdgeFine& ef = sEdgeFine[entry];
        if (ef.stamp == stamp) {
            sLeanPosHit++;
            return ef.fp;
        }
        FinePosOf(pv, &ef.fp);
        ef.stamp = sEdge.e[entry].stamp == stamp ? stamp : 0u;
        sLeanPosMiss++;
        return ef.fp;
    }
    FinePosOf(pv, scratch);
    sLeanPosMiss++;
    return *scratch;
}

/* The near polygon and its fine vertices through the lean clipper; -1 = its pool overflowed (the
 * caller clips the stock way). */
int LeanNearPoly(const Run& r, const uint8_t s3[3], uint32_t mask, FineVtx* fv, BoundV* corners, uint32_t* bound) {
    const PolyV in[3] = { PolyOf(r.vs, sSlot[s3[0]]), PolyOf(r.vs, sSlot[s3[1]]), PolyOf(r.vs, sSlot[s3[2]]) };
    const uint32_t key[3] = { sSlot[s3[0]].posVer, sSlot[s3[1]].posVer, sSlot[s3[2]].posVer };
    constexpr int kPool = 16;
    PolyV pool[kPool];
    const PolyV* poly[GdxTrkRr::kClipPolyMax];
    uint32_t outKey[GdxTrkRr::kClipPolyMax];
    int outEntry[GdxTrkRr::kClipPolyMax];
    uint32_t outStamp[GdxTrkRr::kClipPolyMax];
    const int n = GdxTrkRr::LeanClipPoly(sPass.clip, in, key, sPass.serial, &sEdge, pool, kPool, poly, outKey, outEntry,
                                         outStamp, mask);
    if (n <= 0) {
        return n;
    }
    *bound = 0;
    for (int i = 0; i < n; i++) {
        int slot = -1;
        for (int j = 0; j < 3 && outKey[i] != 0; j++) {
            slot = poly[i] == &in[j] ? (int)s3[j] : slot;
        }
        FinePos scratch;
        const FinePos& fp = CornerPos(*poly[i], slot, outEntry[i], outStamp[i], &scratch);
        memcpy(fv[i].p, fp.p, sizeof(fv[i].p));
        FineAttrOf(*poly[i], &fv[i]);
        corners[i] = fp.bv;
        const float px = fp.eow * 200.0f * 1000.0f;
        const uint32_t bv = px > 4.0e9f ? 4000000000u : (uint32_t)px;
        *bound = bv > *bound ? bv : *bound;
    }
    return n;
}

bool FineReserve(Run& r, uint32_t n) {
    FineSeg& f = r.f;
    if (f.stream != nullptr && f.n + n <= f.cap && f.nIdx + 3 * (n - 2) <= kMaxFineIdx) {
        return true;
    }
    FlushFine(r);
    if (r.I->PendingTris() != 0) {
        r.I->Flush();
    }
    /* the pool reserves in 16-byte units */
    const uint32_t units = (kFineChunk * (uint32_t)sizeof(FineVtx) + 15u) / 16u;
    f.stream = static_cast<FineVtx*>(gdx3ds_mesh_backend_xf_vertices(units));
    f.cap = f.stream != nullptr ? kFineChunk : 0;
    f.n = 0;
    return f.stream != nullptr;
}

void BuildFineXf(Run& r) {
    r.f.xf = r.g.xf; /* same matrix and uniforms; the vertex layout, the UV slope and fog differ */
    if (r.vs.fog) {
        r.f.xf.fog[2] = 0.0f; /* the corners carry the CPU's fog byte in cn.a (see PolyOf) */
        r.f.xf.fog[3] = 1.0f;
    }
    for (int t = 0; t < 2; t++) {
        r.f.xf.uva[t][0] = r.g.xf.uva[t][0] / kFineUv;
        r.f.xf.uva[t][1] = r.g.xf.uva[t][1] / kFineUv;
    }
    r.f.xfValid = true;
}

void FineCensus(const FineVtx& d) {
    const float(*m)[4] = sPass.mpRel;
    const double x = ((double)d.p[0] * m[0][0] + (double)d.p[1] * m[1][0] + (double)d.p[2] * m[2][0] + m[3][0]) * sPass.xs;
    const double y = (double)d.p[0] * m[0][1] + (double)d.p[1] * m[1][1] + (double)d.p[2] * m[2][1] + m[3][1];
    const double w = (double)d.p[0] * m[0][3] + (double)d.p[1] * m[1][3] + (double)d.p[2] * m[2][3] + m[3][3];
    gdx3ds_xf_census_path = GDX3DS_XFC_TRK;
    gdx3ds_xf_census_note_f(d.p, m, sPass.xs, (float)x, (float)y, (float)w);
    gdx3ds_xf_census_path = GDX3DS_XFC_DL;
}

/* A triangle that reaches the near plane (or, with side clipping, a side plane): clipped here, in
 * rebased object space, and drawn by the PICA as a triangle fan in the fine draw. The CPU clipper
 * would have produced the same polygon; the PICA never has to clip at the near plane, where
 * float24 error is divided by the smallest w of the scene. Edges shared with the coarse draw seal
 * (see the FineVtx comment). */
/* The near polygon and its fine vertices, the stock way. */
int StockNearPoly(const Run& r, const uint8_t s3[3], uint32_t mask, FineVtx* fv, BoundV* corners, uint32_t* bound) {
    const PolyV in[3] = { PolyOf(r.vs, sSlot[s3[0]]), PolyOf(r.vs, sSlot[s3[1]]), PolyOf(r.vs, sSlot[s3[2]]) };
    PolyV poly[GdxTrkRr::kClipPolyMax];
    const int n = ClipPoly(sPass.clip, in, poly, mask);
    if (n < 3) {
        return 0;
    }
    *bound = 0;
    for (int i = 0; i < n; i++) {
        float eow = 0.0f;
        FineVertex(poly[i], &fv[i], &eow, &corners[i]);
        const float px = eow * 200.0f * 1000.0f;
        const uint32_t bv = px > 4.0e9f ? 4000000000u : (uint32_t)px;
        *bound = bv > *bound ? bv : *bound;
    }
    return n;
}

void ClipTri(Run& r, uint8_t a, uint8_t b, uint8_t d, uint32_t mask) {
    RunCtx& c = r.c;
    const uint8_t s3[3] = { a, b, d };
    FineVtx fv[GdxTrkRr::kClipPolyMax];
    BoundV corners[GdxTrkRr::kClipPolyMax];
    uint32_t bound = 0;
    int n = sCfg.nearClip == 1 ? LeanNearPoly(r, s3, mask, fv, corners, &bound) : -1;
    if (n < 0) {
        n = StockNearPoly(r, s3, mask, fv, corners, &bound);
    }
    if (n < 3) {
        c.stats_tris++;
        sWin.nearDropped++;
        return;
    }
    if (sCfg.maxErr >= 0 && bound > (uint32_t)sCfg.maxErr) {
        bool visible = true;
        bound = VisibleBoundMpx(corners, n, &visible);
        if (!visible) {
            c.stats_tris++;
            sWin.dropTris++; /* the viewport shows none of it: the CPU clipper would emit nothing */
            return;
        }
    }
    NoteBound(bound);
    if (sCfg.maxErr >= 0 && bound > (uint32_t)sCfg.maxErr) {
        sWin.fbErr++;
        CpuTri(r, a, b, d);
        return;
    }
    FlushRes(r);
    FlushGpu(r.g);
    if (Gdx_NativeBatchOpen()) {
        GdxNativeTri::FlushBatchKeepState(c);
    }
    if (!FineReserve(r, (uint32_t)n)) {
        sWin.fbPool++;
        CpuTri(r, a, b, d);
        return;
    }
    if (!r.f.xfValid) {
        BuildFineXf(r);
    }
    FineSeg& f = r.f;
    const uint16_t base = (uint16_t)f.n;
    for (int i = 0; i < n; i++) {
        f.stream[f.n++] = fv[i];
        if (gdx3ds_xf_census_on) {
            FineCensus(fv[i]);
        }
    }
    if (f.nIdx == 0) {
        f.clsW = 1.5f;
    }
    for (int i = 1; i + 1 < n; i++) {
        f.idx[f.nIdx++] = base;
        f.idx[f.nIdx++] = (uint16_t)(base + i);
        f.idx[f.nIdx++] = (uint16_t)(base + i + 1);
    }
    if (bound > sWin.maxErrMpx) {
        sWin.maxErrMpx = bound;
    }
    if (bound > sMaxErrRun) {
        sMaxErrRun = bound;
    }
    c.stats_tris++;
    sWin.gpuTris++;
    if ((mask & GdxTrkRr::kPlaneNearBit) != 0) {
        sWin.nearTris++;
    }
    if ((mask & GdxTrkRr::kPlaneSideBits) != 0) {
        sWin.sideTris++;
    }
    sWin.fineVerts += (uint32_t)n;
}

/* The planes a corner is outside of: the near plane always, the side planes when the backend's
 * scissor cannot bound the viewport (a folded sub-window viewport: 4P split screen). */
uint32_t ClipMask(const Slot& v, bool sideClip) {
    uint32_t m = 0;
    if (PlaneDist(v.c, GdxTrkRr::PLANE_NEAR) < 0.0) {
        m |= GdxTrkRr::kPlaneNearBit;
    }
    if (sideClip) {
        for (int plane = GdxTrkRr::PLANE_LEFT; plane < GdxTrkRr::PLANE_COUNT; plane++) {
            if (PlaneDist(v.c, plane) < 0.0) {
                m |= 1u << plane;
            }
        }
    }
    return m;
}

void RetainedTri(Run& r, uint8_t a, uint8_t b, uint8_t d) {
    RunCtx& c = r.c;
    GpuSeg& g = r.g;
    if (r.forceCpu) {
        CpuTri(r, a, b, d);
        return;
    }
    if (!SlotLive(a) || !SlotLive(b) || !SlotLive(d)) {
        sWin.fbSlot++;
        CpuTri(r, a, b, d);
        return;
    }
    Slot& v1 = sSlot[a];
    Slot& v2 = sSlot[b];
    Slot& v3 = sSlot[d];
    Screen(r.vs, v1);
    Screen(r.vs, v2);
    Screen(r.vs, v3);
    if (PreDrop(r, v1, v2, v3)) {
        c.stats_tris++;
        sWin.dropTris++;
        return;
    }
    if (c.needExact) {
        const bool straddles = (v1.clipOut | v2.clipOut | v3.clipOut) != 0;
        if (sCfg.level == LEVEL_PARITY && straddles) {
            sWin.fbFirst++; /* the CPU clipper draws it AND resolves the segment's state */
            CpuTri(r, a, b, d);
            return;
        }
        const int tcProbe = TcEnter(TC_PROBE);
        const ProbeResult pr = ProbeState(r, a, b, d);
        TcLeave(tcProbe);
        if (pr == PROBE_DROPPED) {
            c.stats_tris++;
            sWin.dropTris++;
            return;
        }
        if (pr == PROBE_DRAWN) {
            c.stats_tris++;
            sWin.fbFirst++;
            sWin.cpuTris++;
            return;
        }
        /* PROBE_RESOLVED: the state is bound; this tri joins the GPU list below */
    }
    if (!c.prepValid) {
        const int tcPrep = TcEnter(TC_STATE);
        if (!GdxNativeTri::DerivePrep(c)) {
            sWin.fbState++;
            CpuTri(r, a, b, d);
            TcLeave(tcPrep);
            return;
        }
        g.xfValid = BuildXf(g, c, r.vs);
        r.f.xfValid = false;
        TcLeave(tcPrep);
    }
    if (!g.xfValid) {
        sWin.fbState++;
        CpuTri(r, a, b, d);
        return;
    }
    if (sCfg.level == LEVEL_PARITY && (v1.clipOut | v2.clipOut | v3.clipOut) != 0) {
        sWin.fbClip++;
        CpuTri(r, a, b, d);
        return;
    }
    if (sCfg.level == LEVEL_PERF) {
        const uint32_t mask = ClipMask(v1, g.sideClip) | ClipMask(v2, g.sideClip) | ClipMask(v3, g.sideClip);
        if (mask != 0) {
            const int tcNear = TcEnter(TC_NEAR);
            ClipTri(r, a, b, d, mask);
            TcLeave(tcNear);
            return;
        }
    }
    if (!v1.rangeOk || !v2.rangeOk || !v3.rangeOk) {
        sWin.fbRange++;
        CpuTri(r, a, b, d);
        return;
    }
    const int tcGate = TcEnter(TC_GATE);
    uint32_t bound = TriBoundMpx(v1, v2, v3);
    if (sCfg.maxErr >= 0 && bound > (uint32_t)sCfg.maxErr) {
        const BoundV corners[3] = { BoundOf(v1), BoundOf(v2), BoundOf(v3) };
        bool visible = true;
        bound = VisibleBoundMpx(corners, 3, &visible);
        if (!visible) {
            c.stats_tris++;
            sWin.dropTris++; /* the viewport shows none of it: the CPU clipper would emit nothing */
            TcLeave(tcGate);
            return;
        }
    }
    NoteBound(bound);
    TcLeave(tcGate);
    if (sCfg.maxErr >= 0 && bound > (uint32_t)sCfg.maxErr) {
        sWin.fbErr++;
        CpuTri(r, a, b, d);
        return;
    }
    uint16_t i1;
    uint16_t i2;
    uint16_t i3;
    if (g.nIdx + 3 > kMaxIdx) {
        FlushGpu(g);
    }
    if (!StreamIndex(g, a, &i1) || !StreamIndex(g, b, &i2) || !StreamIndex(g, d, &i3)) {
        sWin.fbPool++;
        CpuTri(r, a, b, d);
        return;
    }
    if (Gdx_NativeBatchOpen()) {
        const int tcFlush = TcEnter(TC_SUBMIT);
        GdxNativeTri::FlushBatchKeepState(c); /* painter's order: CPU tris before these */
        TcLeave(tcFlush);
    }
    FlushRes(r);
    FlushFine(r);
    const int tcIdx = TcEnter(TC_STREAM);
    if (g.nIdx == 0) {
        g.clsW = v1.w != 1.0f ? v1.w : 1.5f;
    }
    sIdx[g.nIdx++] = i1;
    sIdx[g.nIdx++] = i2;
    sIdx[g.nIdx++] = i3;
    TcLeave(tcIdx);
    if (bound > sWin.maxErrMpx) {
        sWin.maxErrMpx = bound;
    }
    if (bound > sMaxErrRun) {
        sMaxErrRun = bound;
    }
    c.stats_tris++;
    sWin.gpuTris++;
}

void RetainedTemplate(Run& r, const GdxTrkTemplate& t) {
    int ev = 0;
    for (int i = 0; i < t.ntris; i++) {
        while (ev < t.nevents && t.events[ev].beforeTri == i) {
            const int tcEv = TcEnter(TC_STATE);
            FlushAllGpu(r);
            GdxNativeTri::FlushNative(r.c); /* the combiner changes: draw what precedes it with the old state */
            r.I->SetCombineRaw(t.events[ev].w0, t.events[ev].w1);
            r.c.stats_combines++;
            sWin.events++;
            r.g.xfValid = false;
            r.f.xfValid = false;
            ev++;
            TcLeave(tcEv);
        }
        RetainedTri(r, t.idx[i * 3 + 0], t.idx[i * 3 + 1], t.idx[i * 3 + 2]);
    }
    while (ev < t.nevents) {
        const int tcEv = TcEnter(TC_STATE);
        FlushAllGpu(r);
        GdxNativeTri::FlushNative(r.c);
        r.I->SetCombineRaw(t.events[ev].w0, t.events[ev].w1);
        r.c.stats_combines++;
        sWin.events++;
        r.g.xfValid = false;
        r.f.xfValid = false;
        ev++;
        TcLeave(tcEv);
    }
}

/* ---- [native] track_resident: a template op from the resident buffer ----------------------
 *
 * The op's two rings (slots 0-7 and 8-15) are looked up in the bake by content: positions by
 * hash, then colour and the FINAL short U/V the slots hold now (the walk's scale or its ST
 * patch). A ring found byte for byte is the same vertex the stream would carry, so the op can be
 * drawn by index. Then one check per ring and pass replaces the per-vertex screen, rebase and
 * per-triangle bound: the ring's bounding sphere must be in front of the near plane (else the
 * per-frame path clips it on the CPU), inside F3DEX2.Rej's 2w box when that ucode is loaded
 * (else the CPU path would drop some of its triangles), inside the side planes when the viewport
 * is folded (4P), and its float24 bound -- gpuxf's 2^-14 (sum |m_k| max|ob_k - E_k| + |m_3|) per
 * row over the ring's box, divided by the sphere's smallest w -- must pass the gate. Every
 * triangle of a passing ring is then one the per-frame path would also hand to the PICA whole
 * (the bound is an upper bound of its per-vertex one), except triangles wholly outside one
 * plane, which the PICA clips to nothing, and back faces, which it culls by the same rule. */
enum ResMiss { RES_OK = 0, RES_MISS_SLOT = -1, RES_MISS_RING = -2, RES_MISS_UV = -3 };

struct HalfRes {
    uint32_t pass;
    uint32_t gen;
    uint32_t stamp; /* sVerCounter when resolved: a slot loaded or patched since has ver > stamp */
    int32_t ring;
};
HalfRes sHalf[2] = {};

constexpr uint32_t kResMaxChunks = 1024; /* SEGMENT_CHUNK_COUNT */
struct ChunkChk {
    uint32_t pass;
    uint8_t flags; /* which planes this entry was evaluated with: bit0 side, bit1 rej */
    int8_t why;    /* 0 ok, else a ResWhy */
    uint8_t censused;
    uint8_t pad;
    uint32_t bound; /* mpx */
    float w;        /* centre w: the draw's stereo/DISP classification input */
};
ChunkChk sChk[kResMaxChunks];

enum ResWhy { WHY_OK = 0, WHY_NEAR = 1, WHY_REJ = 2, WHY_SIDE = 3, WHY_ERR = 4 };

/* Per pass: the plane gradients' norms (GdxTrkRr::RingNormsOf; independent of the origin). */
uint32_t sNormsPass = 0;
GdxTrkRr::RingNorms sNorms = {};

int WhyOf(int ring) {
    return ring == GdxTrkRr::RING_NEAR ? WHY_NEAR : (ring == GdxTrkRr::RING_REJ ? WHY_REJ : WHY_SIDE);
}

/* The sphere first (one transform); a ring it cannot clear is decided exactly on its 8 vertices. */
int CheckChunk(const GdxTrkResSet& set, uint32_t chunk, bool side, bool rej) {
    ChunkChk& k = sChk[chunk];
    const uint8_t flags = (uint8_t)((side ? 1u : 0u) | (rej ? 2u : 0u));
    if (k.pass == sPass.serial && k.flags == flags) {
        return k.why;
    }
    if (sNormsPass != sPass.serial) {
        sNormsPass = sPass.serial;
        GdxTrkRr::RingNormsOf(sPass.clip, &sNorms);
    }
    const GdxTrkResChunk& ch = set.chunks[chunk];
    k.pass = sPass.serial;
    k.flags = flags;
    k.censused = 0;
    k.bound = 0;
    const double rel[3] = { (double)ch.c[0] - sPass.origin[0], (double)ch.c[1] - sPass.origin[1],
                            (double)ch.c[2] - sPass.origin[2] };
    double wMin = 0.0;
    double wCentre = 1.0;
    const int maxErr = GdxTrkRes::MaxErr();
    const int planes = GdxTrkRr::RingPlanes(sPass.clip, sNorms, rel, (double)ch.r, side, rej, &wMin, &wCentre);
    k.w = (float)wCentre;
    if (planes == GdxTrkRr::RING_OK) {
        float a[3];
        for (int c = 0; c < 3; c++) {
            const float lo = std::fabs((float)((int32_t)ch.lo[c] - sPass.origin[c]));
            const float hi = std::fabs((float)((int32_t)ch.hi[c] - sPass.origin[c]));
            a[c] = lo > hi ? lo : hi;
        }
        k.bound = GdxTrkRr::RingBoundMpx(sPass.mpRel, sPass.xs, a, wMin);
        if (maxErr < 0 || k.bound <= (uint32_t)maxErr) {
            k.why = WHY_OK;
            return k.why;
        }
    }
    sWin.exactChecks++;
    int32_t v[8][3];
    const Gdx3dsVtxIn* rv = &set.verts[ch.firstRing * 8u];
    for (int i = 0; i < 8; i++) {
        for (int c = 0; c < 3; c++) {
            v[i][c] = (int32_t)rv[i].ob[c] - sPass.origin[c];
        }
    }
    uint32_t bound = 0;
    const int exact = GdxTrkRr::RingPlanesVerts(sPass.clip, sPass.mpRel, sPass.xs, v, 8, side, rej, &wMin, &bound);
    if (exact != GdxTrkRr::RING_OK) {
        k.why = WhyOf(exact);
        return k.why;
    }
    k.bound = bound;
    k.why = (maxErr >= 0 && bound > (uint32_t)maxErr) ? WHY_ERR : WHY_OK;
    return k.why;
}

void CountMiss(int code) {
    if (code == RES_MISS_SLOT) {
        sWin.missSlot++;
    } else if (code == RES_MISS_RING) {
        sWin.missRing++;
    } else if (code == RES_MISS_UV) {
        sWin.missUv++;
    } else if (code == WHY_NEAR) {
        sWin.missNear++;
    } else if (code == WHY_REJ) {
        sWin.missRej++;
    } else if (code == WHY_SIDE) {
        sWin.missSide++;
    } else if (code == WHY_ERR) {
        sWin.missErr++;
    }
}

/* The resident ring the 8 slots of half h hold now, by content; < 0 = a ResMiss. */
int32_t FindRing(const GdxTrkResSet& set, int h) {
    const int s0 = h * 8;
    const Gdx3dsVtxIn* v[8];
    for (int k = 0; k < 8; k++) {
        if (!SlotLive(s0 + k)) {
            return RES_MISS_SLOT;
        }
        v[k] = sSlot[s0 + k].src;
    }
    const int32_t chunk = GdxTrkRes::FindChunk(set, v);
    if (chunk < 0 || (uint32_t)chunk >= kResMaxChunks) {
        return RES_MISS_RING;
    }
    int16_t uv[8][2];
    for (int k = 0; k < 8; k++) {
        SlotUv(sSlot[s0 + k], &uv[k][0], &uv[k][1]);
    }
    const GdxTrkResChunk& ch = set.chunks[chunk];
    for (uint32_t ring = ch.firstRing; ring < ch.firstRing + ch.nRings; ring++) {
        const Gdx3dsVtxIn* rv = &set.verts[ring * 8u];
        bool same = true;
        for (int k = 0; k < 8 && same; k++) {
            same = rv[k].tc[0] == uv[k][0] && rv[k].tc[1] == uv[k][1] && memcmp(rv[k].cn, v[k]->cn, 4) == 0;
        }
        if (same) {
            sWin.kind[set.rings[ring].kind & 15u]++;
            return (int32_t)ring;
        }
    }
    return RES_MISS_UV;
}

int32_t ResolveHalf(const GdxTrkResSet& set, int h) {
    HalfRes& hr = sHalf[h];
    if (hr.pass == sPass.serial && hr.gen == set.generation && hr.stamp != 0) {
        bool fresh = true;
        for (int k = 0; k < 8 && fresh; k++) {
            fresh = sSlot[h * 8 + k].ver <= hr.stamp;
        }
        if (fresh) {
            return hr.ring;
        }
    }
    hr.pass = sPass.serial;
    hr.gen = set.generation;
    hr.stamp = sVerCounter;
    hr.ring = FindRing(set, h);
    return hr.ring;
}

/* Templates whose triangles use only the two rings (slots 0-15); the pipe/tunnel join templates
 * also read the five join vertices (slots 16-20) and stay on the per-frame path. */
bool TemplateIsPair(int t) {
    static int8_t sPair[GDXTRK_TEMPLATE_COUNT];
    static bool sInit = false;
    if (!sInit) {
        for (int i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
            const GdxTrkTemplate& tp = gGdxTrkTemplate[i];
            bool pair = tp.ntris > 0;
            for (int j = 0; j < tp.ntris * 3 && pair; j++) {
                pair = tp.idx[j] < 16;
            }
            sPair[i] = pair ? 1 : 0;
        }
        sInit = true;
    }
    return sPair[t] != 0;
}

void ResCensusRing(const GdxTrkResSet& set, int32_t ring) {
    const float(*m)[4] = sPass.mpRel;
    for (int k = 0; k < 8; k++) {
        const Gdx3dsVtxIn& v = set.verts[(uint32_t)ring * 8u + k];
        int32_t r32[3];
        bool fits = true;
        for (int c = 0; c < 3; c++) {
            r32[c] = (int32_t)v.ob[c] - sPass.origin[c];
            fits = fits && r32[c] >= -32768 && r32[c] <= 32767;
        }
        if (!fits) {
            continue; /* float24-exact all the same (|ob - E| < 2^17); the census takes s16 only */
        }
        const int16_t r[3] = { (int16_t)r32[0], (int16_t)r32[1], (int16_t)r32[2] };
        const double x = ((double)r[0] * m[0][0] + (double)r[1] * m[1][0] + (double)r[2] * m[2][0] + m[3][0]) * sPass.xs;
        const double y = (double)r[0] * m[0][1] + (double)r[1] * m[1][1] + (double)r[2] * m[2][1] + m[3][1];
        const double w = (double)r[0] * m[0][3] + (double)r[1] * m[1][3] + (double)r[2] * m[2][3] + m[3][3];
        gdx3ds_xf_census_path = GDX3DS_XFC_TRK;
        gdx3ds_xf_census_note(r, m, sPass.xs, (float)x, (float)y, (float)w);
        gdx3ds_xf_census_path = GDX3DS_XFC_DL;
    }
}

struct ResOp {
    const GdxTrkResSet* set;
    int32_t ring[2];
    uint32_t chunk[2];
    uint16_t base[2];
    bool side;
    bool verify;
    float clsW;
    uint8_t fail; /* track_opfold: halves whose ring check failed (a mixed op); 0 = a resident op */
};

/* track_resident_verify: every vertex of a resident triangle, as the GPU reads it from the linear
 * copy, against the walk's emission (position, final U/V, colour). Must stay 0. */
void VerifyTri(const Run& r, const ResOp& op, const uint8_t* slots) {
    for (int j = 0; j < 3; j++) {
        const Slot& sl = sSlot[slots[j]];
        const Gdx3dsVtxIn& gv = r.res->linear[op.base[slots[j] >> 3] + (slots[j] & 7u)];
        int16_t u;
        int16_t v;
        SlotUv(sl, &u, &v);
        const bool same = memcmp(gv.ob, sl.src->ob, sizeof(gv.ob)) == 0 && gv.tc[0] == u && gv.tc[1] == v &&
                          memcmp(gv.cn, sl.src->cn, 4) == 0;
        sWin.verifyN++;
        if (!same) {
            sWin.verifyBad++;
        }
    }
}

bool RecheckSide(const Run& r, ResOp& op) {
    for (int h = 0; h < 2; h++) {
        const int why = CheckChunk(*op.set, op.chunk[h], true, r.c.ps.rejectVariant != 0);
        if (why != WHY_OK) {
            return false;
        }
    }
    op.side = true;
    return true;
}

void ResTri(Run& r, ResOp& op, uint8_t a, uint8_t b, uint8_t d) {
    RunCtx& c = r.c;
    GpuSeg& g = r.g;
    if (c.needExact) {
        /* the segment's state is resolved on its first SURVIVING triangle, exactly as the
           per-frame path does: its cheap drop screen first (no interpreter round trip) */
        Screen(r.vs, sSlot[a]);
        Screen(r.vs, sSlot[b]);
        Screen(r.vs, sSlot[d]);
        if (PreDrop(r, sSlot[a], sSlot[b], sSlot[d])) {
            c.stats_tris++;
            sWin.dropTris++;
            return;
        }
        const int tcProbe = TcEnter(TC_PROBE);
        const ProbeResult pr = ProbeState(r, a, b, d);
        TcLeave(tcProbe);
        if (pr == PROBE_DROPPED) {
            c.stats_tris++;
            sWin.dropTris++;
            return;
        }
        if (pr == PROBE_DRAWN) {
            c.stats_tris++;
            sWin.fbFirst++;
            sWin.cpuTris++;
            return;
        }
    }
    if (!c.prepValid) {
        const int tcPrep = TcEnter(TC_STATE);
        if (!GdxNativeTri::DerivePrep(c)) {
            sWin.fbState++;
            CpuTri(r, a, b, d);
            TcLeave(tcPrep);
            return;
        }
        g.xfValid = BuildXf(g, c, r.vs);
        r.f.xfValid = false;
        TcLeave(tcPrep);
    }
    if (!g.xfValid) {
        sWin.fbState++;
        CpuTri(r, a, b, d);
        return;
    }
    if (g.sideClip && !op.side && !RecheckSide(r, op)) {
        RetainedTri(r, a, b, d); /* the probe folded the viewport: this ring needs side clipping */
        return;
    }
    if (g.nIdx != 0 || r.f.nIdx != 0) {
        FlushGpu(g);
        FlushFine(r);
    }
    if (Gdx_NativeBatchOpen()) {
        GdxNativeTri::FlushBatchKeepState(c); /* painter's order: CPU tris before these */
    }
    if (sResN + 3 > kMaxIdx) {
        FlushRes(r);
    }
    if (sResN == 0) {
        sResClsW = op.clsW != 1.0f ? op.clsW : 1.5f;
    }
    const uint8_t slots[3] = { a, b, d };
    for (int j = 0; j < 3; j++) {
        sResIdx[sResN++] = (uint16_t)(op.base[slots[j] >> 3] + (slots[j] & 7u));
    }
    if (op.verify) {
        VerifyTri(r, op, slots);
    }
    c.stats_tris++;
    sWin.gpuTris++;
    sWin.resTris++;
}

/* ---- [native] track_opfold: fewer per-frame template ops (C5 Stage 3d) --------------------------
 *
 * 1. The fold. Once a resident op's segment state is resolved (no probe pending, the prep and the
 *    uniforms valid, no other draw open, the viewport as the op was checked with), every triangle
 *    up to the template's next combiner event would take ResTri's same final branch: three indices
 *    into the open resident draw. The fold appends them in one loop (the same indices, the same
 *    order, the same counters).
 * 2. Mixed ops. c5s3c sends a whole op to the per-frame path when one of its two rings fails the
 *    ring check (reaches the near plane, the 2w box, a folded side plane, or exceeds the float24
 *    gate at a corner): the camera's own chunks, ~3-5 ops a frame. But both rings were found in the
 *    bake, and the ring that passed is proved clear of every plane with every vertex's bound at
 *    most its ring bound (<= the gate). So per triangle the per-frame path's decision needs only
 *    the failing ring's vertices, screened exactly as it screens them: a triangle with a failing
 *    corner outside a clip plane, out of s16 range or over the gate goes to RetainedTri (the
 *    per-frame path itself, unchanged); one that F3DEX2.Rej drops is dropped; the rest -- the
 *    per-frame path would stream them whole to the PICA -- are drawn by resident index (the PICA
 *    input is bit-identical, c5s3c-progress.md §2.3). The passing ring's 8 vertices are never
 *    screened, rebased or streamed. As in a resident op, a triangle wholly outside one plane is
 *    left to the PICA, which clips it to nothing. */
void ResAppend(Run& r, const ResOp& op, uint8_t a, uint8_t b, uint8_t d) {
    RunCtx& c = r.c;
    GpuSeg& g = r.g;
    if (g.nIdx != 0 || r.f.nIdx != 0) {
        FlushGpu(g);
        FlushFine(r);
    }
    if (Gdx_NativeBatchOpen()) {
        GdxNativeTri::FlushBatchKeepState(c); /* painter's order: CPU tris before these */
    }
    if (sResN + 3 > kMaxIdx) {
        FlushRes(r);
    }
    if (sResN == 0) {
        sResClsW = op.clsW != 1.0f ? op.clsW : 1.5f;
    }
    const uint8_t slots[3] = { a, b, d };
    for (int j = 0; j < 3; j++) {
        sResIdx[sResN++] = (uint16_t)(op.base[slots[j] >> 3] + (slots[j] & 7u));
    }
    if (op.verify) {
        VerifyTri(r, op, slots);
    }
    c.stats_tris++;
    sWin.gpuTris++;
    sWin.resTris++;
}

/* The resolved segment state lets the rest of a resident op's triangles append without a check. */
bool FoldReady(const Run& r, const ResOp& op) {
    return !r.c.needExact && r.c.prepValid && r.g.xfValid && !(r.g.sideClip && !op.side) && r.g.nIdx == 0 &&
           r.f.nIdx == 0 && !Gdx_NativeBatchOpen() && !op.verify;
}

/* Triangles [from, to) of template t, appended in one loop (FoldReady held). */
void FoldAppend(Run& r, const ResOp& op, const GdxTrkTemplate& t, int from, int to) {
    const uint32_t n = (uint32_t)(to - from);
    if (sResN + 3u * n > kMaxIdx) {
        FlushRes(r);
    }
    if (sResN == 0) {
        sResClsW = op.clsW != 1.0f ? op.clsW : 1.5f;
    }
    const uint8_t* idx = &t.idx[from * 3];
    for (uint32_t j = 0; j < 3u * n; j++) {
        sResIdx[sResN++] = (uint16_t)(op.base[idx[j] >> 3] + (idx[j] & 7u));
    }
    r.c.stats_tris += n;
    sWin.gpuTris += n;
    sWin.resTris += n;
    sWin.foldRuns++;
    sWin.foldTris += n;
}

void MixCensus(Slot& sl) {
    if (gdx3ds_xf_census_on && sl.censusVer != sl.posVer && sl.rangeOk) {
        sl.censusVer = sl.posVer;
        CensusNote(sl);
    }
}

void MixTri(Run& r, ResOp& op, uint8_t a, uint8_t b, uint8_t d) {
    RunCtx& c = r.c;
    GpuSeg& g = r.g;
    if (c.needExact || !c.prepValid || !g.xfValid || (g.sideClip && !op.side)) {
        RetainedTri(r, a, b, d); /* a segment start or a state change: the per-frame path, unchanged */
        sWin.mixFrameTris++;
        return;
    }
    const uint8_t s3[3] = { a, b, d };
    const bool rej = c.ps.rejectVariant != 0;
    uint32_t mask = 0;
    uint32_t bound = 0;
    bool range = true;
    bool rejDrop = false;
    for (int j = 0; j < 3; j++) {
        const int h = s3[j] >> 3;
        if ((op.fail & (1u << h)) == 0) {
            const uint32_t rb = sChk[op.chunk[h]].bound;
            bound = rb > bound ? rb : bound;
            continue;
        }
        Slot& v = sSlot[s3[j]];
        Screen(r.vs, v);
        mask |= ClipMask(v, g.sideClip);
        range = range && v.rangeOk;
        rejDrop = rejDrop || (rej && RejBox(v));
        const uint32_t vb = BoundMpx(v.errXY, v.wr);
        bound = vb > bound ? vb : bound;
    }
    if (rejDrop) {
        c.stats_tris++;
        sWin.dropTris++; /* PreDrop's F3DEX2.Rej box: the per-frame path drops it before anything else */
        return;
    }
    if (mask != 0 || !range || (sCfg.maxErr >= 0 && bound > (uint32_t)sCfg.maxErr)) {
        RetainedTri(r, a, b, d);
        sWin.mixFrameTris++;
        return;
    }
    for (int j = 0; j < 3; j++) {
        if ((op.fail & (1u << (s3[j] >> 3))) != 0) {
            MixCensus(sSlot[s3[j]]);
        }
    }
    NoteBound(bound);
    ResAppend(r, op, a, b, d);
    sWin.mixResTris++;
}

void TemplateEvent(Run& r, const GdxTrkTemplate& t, int ev) {
    const int tcEv = TcEnter(TC_STATE);
    FlushAllGpu(r);
    GdxNativeTri::FlushNative(r.c); /* the combiner changes: draw what precedes it with the old state */
    r.I->SetCombineRaw(t.events[ev].w0, t.events[ev].w1);
    r.c.stats_combines++;
    sWin.events++;
    r.g.xfValid = false;
    r.f.xfValid = false;
    TcLeave(tcEv);
}

/* A mixed op (one ring failed its check): per triangle, MixTri. */
void MixedTemplate(Run& r, ResOp& op, const GdxTrkTemplate& t) {
    sWin.mixOps++;
    if (gdx3ds_xf_census_on) {
        for (int h = 0; h < 2; h++) {
            if ((op.fail & (1u << h)) == 0 && sChk[op.chunk[h]].censused == 0) {
                sChk[op.chunk[h]].censused = 1;
                ResCensusRing(*op.set, op.ring[h]);
            }
        }
    }
    int ev = 0;
    for (int i = 0; i < t.ntris; i++) {
        while (ev < t.nevents && t.events[ev].beforeTri == i) {
            TemplateEvent(r, t, ev);
            ev++;
        }
        MixTri(r, op, t.idx[i * 3 + 0], t.idx[i * 3 + 1], t.idx[i * 3 + 2]);
    }
    while (ev < t.nevents) {
        TemplateEvent(r, t, ev);
        ev++;
    }
}

/* The op from the resident buffer; false = it takes the per-frame path (nothing drawn yet). */
bool ResidentTemplate(Run& r, int tmplIdx) {
    if (r.res == nullptr || r.forceCpu) {
        return false;
    }
    const int tcRes = TcEnter(TC_RES);
    if (!TemplateIsPair(tmplIdx)) {
        sWin.missJoin++;
        TcLeave(tcRes);
        return false;
    }
    const GdxTrkResSet& set = *r.res->set;
    ResOp op;
    op.set = &set;
    op.side = sCfg.sideClipAlways || gdx3ds_track_backend_viewport_folded() != 0;
    op.verify = GdxTrkRes::Verify();
    const bool rej = r.c.ps.rejectVariant != 0;
    for (int h = 0; h < 2; h++) {
        op.ring[h] = ResolveHalf(set, h);
        if (op.ring[h] < 0) {
            CountMiss(op.ring[h]);
            TcLeave(tcRes);
            return false;
        }
        op.chunk[h] = set.rings[op.ring[h]].chunk;
        op.base[h] = (uint16_t)((uint32_t)op.ring[h] * 8u);
    }
    uint32_t bound = 0;
    op.fail = 0;
    for (int h = 0; h < 2; h++) {
        const int why = CheckChunk(set, op.chunk[h], op.side, rej);
        if (why != WHY_OK) {
            CountMiss(why);
            if (sCfg.opFold == 0) {
                TcLeave(tcRes);
                return false;
            }
            op.fail = (uint8_t)(op.fail | (1u << h));
            continue;
        }
        bound = sChk[op.chunk[h]].bound > bound ? sChk[op.chunk[h]].bound : bound;
    }
    op.clsW = sChk[op.chunk[0]].w;
    if (op.fail != 0) {
        if (op.fail == 3u) {
            TcLeave(tcRes);
            return false;
        }
        MixedTemplate(r, op, gGdxTrkTemplate[tmplIdx]);
        TcLeave(tcRes);
        return true;
    }
    sWin.resOps++;
    sWin.resMaxErr = bound > sWin.resMaxErr ? bound : sWin.resMaxErr;
    if (bound > sMaxErrRun) {
        sMaxErrRun = bound;
    }
    NoteBound(bound);
    if (gdx3ds_xf_census_on) {
        for (int h = 0; h < 2; h++) {
            if (sChk[op.chunk[h]].censused == 0) {
                sChk[op.chunk[h]].censused = 1;
                ResCensusRing(set, op.ring[h]);
            }
        }
    }
    const GdxTrkTemplate& t = gGdxTrkTemplate[tmplIdx]; /* the op stays in TC_RES; nested buckets switch back */
    int ev = 0;
    for (int i = 0; i < t.ntris; i++) {
        while (ev < t.nevents && t.events[ev].beforeTri == i) {
            TemplateEvent(r, t, ev);
            ev++;
        }
        if (sCfg.opFold != 0 && FoldReady(r, op)) {
            const int end = ev < t.nevents ? t.events[ev].beforeTri : t.ntris;
            if (end > i) {
                FoldAppend(r, op, t, i, end);
                i = end - 1;
                continue;
            }
        }
        ResTri(r, op, t.idx[i * 3 + 0], t.idx[i * 3 + 1], t.idx[i * 3 + 2]);
    }
    while (ev < t.nevents) {
        TemplateEvent(r, t, ev);
        ev++;
    }
    TcLeave(tcRes);
    return true;
}

/* Vertex load: latched for the GPU; in a CPU run, also the stock SpVertex (the cache must hold
 * exactly what the CPU replay leaves there). */
void OpVertex(Run& r, const GdxTrkOp& op) {
    const Gdx3dsVtxIn* src = reinterpret_cast<const Gdx3dsVtxIn*>(static_cast<uintptr_t>(op.arg));
    if (r.forceCpu) {
        r.I->SpVertex(op.n, op.slot, src);
    }
    for (int k = 0; k < op.n && op.slot + k < kSlots; k++) {
        LoadSlot(*r.I, op.slot + k, src + k);
        if (r.forceCpu) {
            sSlot[op.slot + k].interpVer = sSlot[op.slot + k].ver;
        }
    }
}

void OpModify(Run& r, const GdxTrkOp& op) {
    if (op.slot >= kSlots) {
        return;
    }
    const bool wasInterp = sSlot[op.slot].interpVer == sSlot[op.slot].ver;
    ModifySlot(op.slot, op.arg);
    if (r.forceCpu || wasInterp) {
        /* the interpreter's copy was current: patch it in place, as the stock replay does */
        r.I->SpModifyVertex(op.slot, GDXSEAM_G_MWO_POINT_ST, op.arg);
        sSlot[op.slot].interpVer = sSlot[op.slot].ver;
    }
}

uint32_t StreamBound(const GdxTrkRecord* rec, const GdxTrkRun& run) {
    uint32_t n = kSlots; /* slots loaded by an earlier run and first drawn here */
    for (uint32_t k = run.opStart; k < run.opEnd && k < rec->nOps; k++) {
        const GdxTrkOp& op = rec->ops[k];
        if (op.kind == GDXTRK_OP_VTX) {
            n += op.n;
        } else if (op.kind == GDXTRK_OP_MOD) {
            n++;
        }
    }
    return n;
}

float TicksToMs(double ticks) {
    return (float)(ticks / (double)SYSCLOCK_ARM11 * 1000.0);
}

/* [trkcat] receipt: each bucket in ms per rendered frame with its own clock reads' cost taken
 * out (`raw` keeps it), then the game side: the walk and the recorder inside it. */
void EmitTrkCat(uint32_t frame, float f) {
    if (sTc.cfg <= 0) {
        return;
    }
    char buf[640];
    int n = snprintf(buf, sizeof(buf), "[trkcat] frame=%u f=%.0f |", frame, (double)f);
    double sum = 0.0;
    double raw = 0.0;
    uint32_t reads = 0;
    for (int c = 0; c < TC_COUNT && n > 0 && n < (int)sizeof(buf); c++) {
        const double acc = (double)sTc.win.acc[c];
        double net = acc - (double)sTc.win.reads[c] * (double)sTc.readTicks;
        net = net > 0.0 ? net : 0.0;
        sum += net;
        raw += acc;
        reads += sTc.win.reads[c];
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s=%.3f", kTcName[c], (double)(TicksToMs(net) / f));
    }
    const GdxTrkWalkStats w = gdxtrk_walk_stats;
    memset(&gdxtrk_walk_stats, 0, sizeof(gdxtrk_walk_stats));
    /* a timed recorder call contains one clock read: take it out */
    double recPerCall = w.recSamples != 0 ? (double)w.recSampleTicks / (double)w.recSamples - sTc.readTicks : 0.0;
    recPerCall = recPerCall > 0.0 ? recPerCall : 0.0;
    const double recTicks = recPerCall * (double)w.recCalls;
    const float passes = w.passes != 0 ? (float)w.passes : 1.0f;
    if (n > 0 && n < (int)sizeof(buf)) {
        snprintf(buf + n, sizeof(buf) - (size_t)n,
                 " | sum=%.3f raw=%.3f reads/f=%.0f read_us=%.2f | cull/f=%.3f walk/f=%.3f walk/pass=%.3f rec/f=%.3f "
                 "rec_us/op=%.2f ops/pass=%.0f verts/pass=%.0f passes=%u",
                 (double)(TicksToMs(sum) / f), (double)(TicksToMs(raw) / f), (double)(reads / f),
                 (double)(TicksToMs(sTc.readTicks) * 1000.0f), (double)(TicksToMs((double)w.cullTicks) / f),
                 (double)(TicksToMs((double)w.walkTicks) / f),
                 (double)(TicksToMs((double)w.walkTicks) / passes), (double)(TicksToMs(recTicks) / f),
                 (double)(TicksToMs(recPerCall) * 1000.0f),
                 (double)(w.ops / passes), (double)(w.verts / passes), w.passes);
    }
    Log("%s", buf);
    memset(&sTc.win, 0, sizeof(sTc.win));
}

uint32_t KindsWith(uint8_t bit) {
    uint32_t t = 0;
    for (int k = 0; k < 16; k++) {
        t += (k & bit) != 0 ? sWin.kind[k] : 0;
    }
    return t;
}

/* [trkres] frame=: the resident split (template ops drawn by index vs on the per-frame path, and
 * why), the variant kinds the walk actually used, the resident bound, and the verify count. */
void EmitResident(uint32_t frame, float f) {
    if (GdxTrkRes::Level() == 0) {
        return;
    }
    Log("[trkres] frame=%u f=%.0f | res_ops/f=%.2f frame_ops/f=%.2f res_tris/f=%.1f res_draws/f=%.2f res_mpx=%u | "
        "miss join=%u slot=%u ring=%u uv=%u near=%u rej=%u side=%u err=%u | kinds own=%u corr=%u patched=%u "
        "ftable=%u flayout=%u | exact=%u probes/f=%.2f events/f=%.2f | verify=%u/%u",
        frame, (double)f, sWin.resOps / f, sWin.frameOps / f, sWin.resTris / f, sWin.resDraws / f, sWin.resMaxErr,
        sWin.missJoin, sWin.missSlot, sWin.missRing, sWin.missUv, sWin.missNear, sWin.missRej, sWin.missSide,
        sWin.missErr, sWin.kind[0], KindsWith(GDXTRK_RES_CORR), KindsWith(GDXTRK_RES_PATCHED),
        KindsWith(GDXTRK_RES_FTABLE), KindsWith(GDXTRK_RES_FLAYOUT), sWin.exactChecks, sWin.probes / f,
        sWin.events / f, sWin.verifyBad, sWin.verifyN);
}

/* [trkmat] frame=: the course material table (C5 Stage 3d). probes/f = segment resolutions,
 * qual = the probes a record may serve (no clip, nothing pending), served = served from a record
 * (no SpTri), then the lookup results, the banks and invalidations, and verify's comparisons. */
void EmitMat(uint32_t frame, float f) {
    if (sCfg.matTab == 0) {
        return;
    }
    Log("[trkmat] frame=%u f=%.0f lvl=%d live=%d | probes/f=%.2f qual/f=%.2f served/f=%.2f geodrop=%u drop=%u stock=%u nointerp=%u | "
        "res hit=%u empty=%u key=%u noimp=%u tex=%u epoch=%u smp=%u full=%u | banked=%u inval=%u binds=%u live=%u | "
        "vfy checks=%u mism=%u bits=0x%x | recs=%u bytes=%u",
        frame, (double)f, sCfg.matTab, sCfg.matLive ? 1 : 0, sWin.probes / f, sWin.matQual / f, sWin.matServed / f,
        sWin.matGeoDrop, sWin.matDrop, sWin.matStock, sWin.matNoInterp, sWin.matRes[GDXSEAM_MM_HIT], sWin.matRes[GDXSEAM_MM_MISS_EMPTY],
        sWin.matRes[GDXSEAM_MM_MISS_KEY], sWin.matRes[GDXSEAM_MM_MISS_NOIMP], sWin.matRes[GDXSEAM_MM_MISS_TEX],
        sWin.matRes[GDXSEAM_MM_MISS_EPOCH], sWin.matRes[GDXSEAM_MM_MISS_SMP], sWin.matRes[GDXSEAM_MM_FULL],
        sWin.matBanked, sWin.matInval, sWin.matBinds, sWin.matLive, sWin.matChecks, sWin.matMism, sWin.matMismBits,
        (unsigned)(kMatBuckets * kMatWays), (unsigned)sizeof(sMat));
}

/* [trkop] frame=: track_opfold (mixed ops and the folded append) and track_nearclip (the lean
 * clipper's edge-point and fine-position caches) (C5 Stage 3d). */
void EmitOps(uint32_t frame, float f) {
    if (sCfg.opFold == 0 && sCfg.nearClip == 0) {
        return;
    }
    Log("[trkop] frame=%u f=%.0f fold=%d nearclip=%d | mix_ops/f=%.2f mix_res_tris/f=%.1f mix_frame_tris/f=%.1f | "
        "fold runs/f=%.2f tris/f=%.1f | lean edge hit=%u miss=%u pos hit=%u miss=%u",
        frame, (double)f, sCfg.opFold, sCfg.nearClip, sWin.mixOps / f, sWin.mixResTris / f, sWin.mixFrameTris / f,
        sWin.foldRuns / f, sWin.foldTris / f, sEdge.hits, sEdge.misses, sLeanPosHit, sLeanPosMiss);
    sEdge.hits = 0;
    sEdge.misses = 0;
    sLeanPosHit = 0;
    sLeanPosMiss = 0;
}

void EmitWindow(uint32_t frame) {
    const float f = sWin.frames ? (float)sWin.frames : 1.0f;
    const float ms = (float)sWin.ticks / (float)SYSCLOCK_ARM11 * 1000.0f;
    Log("[trkrr] frame=%u course=%d f=%u lvl=%d rebase=%d maxerr=%d | passes/f=%.2f runs/f=%.2f cpuruns=%u | draws/f=%.2f "
        "gputris/f=%.1f cputris/f=%.1f drop/f=%.1f near/f=%.1f side/f=%.1f neardrop/f=%.1f fine=%u/%u | "
        "fb first=%u clip=%u err=%u range=%u state=%u pool=%u slot=%u near=%u | "
        "verts/f=%.1f bytes/f=%.0f resident=0 mat/f=%.1f | err_mpx=%u all_mpx=%u run_mpx=%u "
        "bound<1/64:%u <1/8:%u <1:%u >=1:%u nearW=%.2f | ms/f=%.3f",
        frame, (int)sWin.course, sWin.frames, sCfg.level, sCfg.rebase ? 1 : 0, sCfg.maxErr, sWin.passes / f, sWin.runs / f,
        sWin.cpuRuns, sWin.gpuDraws / f, sWin.gpuTris / f, sWin.cpuTris / f, sWin.dropTris / f, sWin.nearTris / f,
        sWin.sideTris / f, sWin.nearDropped / f, sWin.fineDraws, sWin.fineVerts, sWin.fbFirst, sWin.fbClip, sWin.fbErr, sWin.fbRange,
        sWin.fbState, sWin.fbPool, sWin.fbSlot, sWin.fbNear, sWin.streamVerts / f,
        sWin.streamVerts * 16.0f / f, sWin.materialised / f, sWin.maxErrMpx, sWin.maxErrAll, sMaxErrRun,
        sWin.hist[0], sWin.hist[1], sWin.hist[2], sWin.hist[3], (double)sWin.nearWMin,
        (double)(ms / f));
    EmitResident(frame, f);
    EmitMat(frame, f);
    EmitOps(frame, f);
    EmitTrkCat(frame, f);
    memset(&sWin, 0, sizeof(sWin));
}

void WindowTick(const GdxTrkRecord* rec) {
    if (rec->frame != sWin.lastFrame || sWin.frames == 0) {
        sWin.frames++;
        sWin.lastFrame = rec->frame;
    }
    sWin.course = rec->course;
    const uint32_t key = rec->frame >> 6;
    if (sWinKey == 0xFFFFFFFFu) {
        sWinKey = key;
    }
    if (key != sWinKey) {
        sWinKey = key;
        EmitWindow(rec->frame);
        sWin.frames = 1;
        sWin.lastFrame = rec->frame;
    }
}

} // namespace

void gdx3ds_track_retained_run(GdxInterpSeam& I, GdxTrkRecord* rec, uint32_t run) {
    const uint64_t t0 = svcGetSystemTick();
    LatchConfig();
    if (!sInitLogged) {
        sInitLogged = true;
        Log("[trkrr] init lvl=%d maxerr=%d rebase=%d shader=%d (eye-rebased gpuxf draws of the course runs; "
            "0x60 markers)",
            sCfg.level, sCfg.maxErr, sCfg.rebase ? 1 : 0, gdx3ds_mesh_backend_xf_ready());
    }
    if (sCfg.matTab != 0 && !sMatInitLogged) {
        sMatInitLogged = true;
        Log("[trkmat] init track_mattab=%d (%s) live=%d table=%dx%d rec=%u bytes (the retained course's segment probe "
            "from a course material table; needs track_retained=1 and interp=1)",
            sCfg.matTab, sCfg.matTab == 2 ? "verify" : "use", sCfg.matLive ? 1 : 0, kMatBuckets, kMatWays,
            (unsigned)sizeof(GdxSeamMatRec));
    }
    WindowTick(rec);
    MatCourse(rec->course);
    TcBeginRun(svcGetSystemTick());
    rec->rp.markers++;
    rec->rp.runs++;
    sWin.runs++;
    sRunSerial++;

    Run r;
    memset(&r, 0, sizeof(r));
    r.I = &I;
    r.g.I = &I;
    r.forceCpu = sCfg.level == LEVEL_OFF || !gdx3ds_mesh_backend_xf_ready();
    if (!I.LatchVtxState(r.vs) || r.vs.lighting) {
        r.forceCpu = true;
        sWin.fbState++;
    }
    if (!r.forceCpu && !SamePass(rec, r.vs)) {
        BeginPass(rec, r.vs);
    } else if (r.forceCpu && !SamePass(rec, r.vs)) {
        BeginPass(rec, r.vs); /* slots loaded by this run still belong to a pass */
    }
    const GdxTrkRun& rr = rec->runs[run];
    if (I.PendingTris() != 0) {
        I.Flush(); /* the native batch and the stream must start at the pool cursor, nothing pending */
    }
    if (!r.forceCpu) {
        const uint32_t cap = StreamBound(rec, rr);
        r.g.stream = static_cast<Gdx3dsVtxIn*>(gdx3ds_mesh_backend_xf_vertices(cap));
        r.g.streamCap = cap;
        if (r.g.stream == nullptr) {
            r.forceCpu = true;
            sWin.fbPool++;
        }
    }
    if (r.forceCpu) {
        sWin.cpuRuns++;
    }
    const GdxTrkResView* view = GdxTrkRes::Acquire(rec->frame); /* adopts a new bake (course load) */
    r.res = (sCfg.level == LEVEL_PERF && !r.forceCpu) ? view : nullptr;
    const uint32_t draws0 = sWin.gpuDraws;
    const uint32_t tris0 = sWin.gpuTris;
    GdxNativeTri::Init(r.c, &I, false);
    I.PackState(&r.c.ps); /* the ucode's cull masks and reject variant, before any tri resolves */

    for (uint32_t k = rr.opStart; k < rr.opEnd && k < rec->nOps; k++) {
        const GdxTrkOp& op = rec->ops[k];
        switch (op.kind) {
            case GDXTRK_OP_VTX:
                TcEnter(TC_LATCH);
                rec->rp.vtxOps++;
                rec->rp.verts += op.n;
                OpVertex(r, op);
                break;
            case GDXTRK_OP_MOD:
                TcEnter(TC_LATCH);
                OpModify(r, op);
                break;
            case GDXTRK_OP_DL:
                TcEnter(TC_TRI);
                if (op.tmpl < GDXTRK_TEMPLATE_COUNT && !ResidentTemplate(r, op.tmpl)) {
                    if (r.res != nullptr) {
                        sWin.frameOps++;
                    }
                    RetainedTemplate(r, gGdxTrkTemplate[op.tmpl]);
                }
                break;
            default:
                break;
        }
    }
    TcEnter(TC_SUBMIT);
    FlushAllGpu(r);
    GdxNativeTri::FlushNative(r.c);
    TcEnter(TC_SETUP);

    rec->rp.tris += r.c.stats_tris;
    rec->rp.fast += r.c.stats_fast;
    rec->rp.exact += r.c.stats_exact;
    rec->rp.fallback += r.c.stats_fallback;
    rec->rp.combines += r.c.stats_combines;
    rec->rp.draws += r.c.stats_draws;
    rec->rp.gpuDraws += sWin.gpuDraws - draws0;
    rec->rp.gpuTris += sWin.gpuTris - tris0;
    sWin.ticks += svcGetSystemTick() - t0;
    TcEndRun();
}
