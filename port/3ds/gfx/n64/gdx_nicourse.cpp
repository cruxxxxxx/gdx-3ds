/* port/3ds/gfx/n64/gdx_nicourse.cpp -- nicourse `[native] ni_course` (nointerp S5): the walker side of the
 * 3D course preview (docs/research/native/nicourse-progress.md).
 *
 * The game (decomp-native-course-preview.patch, port/3ds/game/gdx3ds_course_game.c) writes two G_NOOP
 * p=0x6B words right before each course model's G_VTX / G_TRI2 run (Course_DrawModel):
 *   [0] w0 low 15 bits = the run's command count, w1 = the model's vertex buffer (tagged host pointer)
 *   [1] w0 bit 15 set, low 15 bits = the vertex count, w1 = the buffer's content generation.
 * At [0] this hands the run to the mesh module (gdx3ds_mesh_course_seam), which checks every word of it
 * against its baked copy and draws it from a GPU-resident buffer with the GPU transform; then the
 * walker's position moves past [1] and the run. Not taken (a word differs, a model reaching the
 * frustum's edge, a state the shader cannot express): nothing is touched, the walker goes on to [1]
 * (a no-op) and walks the run stock.
 *
 * What the stock walk would leave behind and this path must leave too: the registers (the run writes
 * none), the backend binding (the draw binds through ResolveDrawState, as the run's first triangle
 * would) and the RSP vertex cache, which the run's G_VTX loads fill. The next model's run reloads the
 * cache from slot 0, so the last loads are re-executed only when the run is not followed by another
 * model's marker (the last model of the frame): the final cache is the stock one. */
#include "gdx_interp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_hotcold.h"
#include "gdx3ds_interp_seam.h"
#include "gdx3ds_vtx_math.h"
#include "gdx_seam_owned.h"
#include "../nrr/gdx3ds_nrr.h"
#include "../nrr/gdx3ds_nrr_mach.h" /* gdx3ds_nrr_backend_capture */

extern "C" void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));

namespace gdxn {

namespace {
constexpr uint32_t kCourseMarkerP = 0x6Bu;

inline bool IsCourseMarker(const Cmd& c) {
    return Opcode(c.w0) == OP_NOOP && ((c.w0 >> 16) & 0xFFu) == kCourseMarkerP;
}
} // namespace

/* Another model follows: G_MOVEMEM (its viewport), then its first marker word. */
bool Interp::NrrCourseHotNextIsModel(const Frame& f) const {
    if (f.idx + 2 > f.list.limit) {
        return false;
    }
    const Cmd vp = ReadCmd(f.list, f.idx);
    const Cmd mk = ReadCmd(f.list, f.idx + 1);
    return Opcode(vp.w0) == OP_MOVEMEM && IsCourseMarker(mk) && (mk.w0 & 0x8000u) == 0;
}

namespace {

/* ---- round 2: the material memo ---------------------------------------------------------------
 * Everything ResolveDrawState reads for an untextured, unlit, unfogged gpuxf draw, plus the frame
 * config the vertex math's x mode comes from. Equal key => the cold draw's bind (shader, combiner
 * inputs, depth / blend / alpha, cull) and its run uniforms (conv, colour, selects) are the same, so
 * the hot draw applies the recorded NRR state block (direct words under nrr_direct=1) and uploads
 * the recorded uniform block with this frame's MP matrix in its first four rows. */
struct CourseKey {
    uint32_t combLo, combHi, oml, omh, geo, egeo;
    uint32_t prim, env, fog, blend;
    uint32_t primDepth, variant;
    uint32_t curW, curH, wide;
};

constexpr int kMemos = 4;
struct CourseMemo {
    CourseKey key;
    uint32_t gen; /* NrrStateGeneration at the build */
    const NrrState* block;
    unsigned char* mem; /* NrrStateBytes(), 8-aligned */
    float in[7][4];
    int numInputs;
    float xf[28][4];
    int xMode;
    float aspect;
    uint32_t lastUse;
    bool used;
};
CourseMemo sMemo[kMemos];
uint32_t sMemoUse = 0;
NrrCatStats sCourseStats; /* the hand-offs' core counters (draws, states) */

inline uint32_t Rgba32(const Rgba& c) {
    return (static_cast<uint32_t>(c.r) << 24) | (static_cast<uint32_t>(c.g) << 16) |
           (static_cast<uint32_t>(c.b) << 8) | c.a;
}

constexpr float kCvMargin = 1.0f / 1024.0f; /* gdx3ds_mesh.cpp kXfMargin */
constexpr float kCvMinW = 0.01f;            /* gdx3ds_mesh.cpp kXfMinW */
enum { CV_INSIDE = 0, CV_OUTSIDE = 1, CV_STRADDLE = 2 };

/* XfClipOf / XfClassify / XfErrorBoundMpx of gdx3ds_mesh.cpp on the memo's x mode: the box corners
 * against the clip volume, the smallest w and the float24 bound (mpx). */
inline void NrrCourseHotClip(const float (*mp)[4], int xMode, float aspect, float ox, float oy, float oz, float c[4]) {
    for (int i = 0; i < 4; i++) {
        c[i] = ox * mp[0][i] + oy * mp[1][i] + oz * mp[2][i] + mp[3][i];
    }
    if (xMode == GDX3DS_VTX_X_STRETCH) {
        c[0] = c[0] * (160.0f / (160.0f - 12.0f));
    } else if (xMode == GDX3DS_VTX_X_ADJ) {
        c[0] = c[0] * (4.0f / 3.0f) / aspect;
    }
}

int NrrCourseHotCover(const float (*mp)[4], const CourseMemo& mm, const Gdx3dsCourseModel& m, float* wMin) {
    uint32_t allOut = 0x1Fu;
    bool inside = true;
    *wMin = 1e30f;
    for (int k = 0; k < 8; k++) {
        float c[4];
        NrrCourseHotClip(mp, mm.xMode, mm.aspect, (k & 1) ? m.hi[0] : m.lo[0], (k & 2) ? m.hi[1] : m.lo[1],
                         (k & 4) ? m.hi[2] : m.lo[2], c);
        const float w = c[3];
        const float in = w * (1.0f - kCvMargin);
        const float out = w * (1.0f + kCvMargin);
        *wMin = w < *wMin ? w : *wMin;
        if (!(w > kCvMinW && c[0] > -in && c[0] < in && c[1] > -in && c[1] < in && c[2] > -in && c[2] < in)) {
            inside = false;
        }
        uint32_t o = 0;
        o |= (c[0] < -out) ? 1u : 0u;
        o |= (c[0] > out) ? 2u : 0u;
        o |= (c[1] < -out) ? 4u : 0u;
        o |= (c[1] > out) ? 8u : 0u;
        o |= (c[2] > out) ? 16u : 0u;
        allOut &= (w > 0.0f) ? o : 0u;
    }
    if (inside) {
        return CV_INSIDE;
    }
    return allOut != 0 ? CV_OUTSIDE : CV_STRADDLE;
}

unsigned NrrCourseHotErr(const float (*mp)[4], int xMode, const Gdx3dsCourseModel& m, float wMin) {
    float amax[3];
    for (int a = 0; a < 3; a++) {
        amax[a] = fabsf(m.lo[a]) > fabsf(m.hi[a]) ? fabsf(m.lo[a]) : fabsf(m.hi[a]);
    }
    float e[4];
    for (int i = 0; i < 4; i++) {
        e[i] = fabsf(mp[3][i]);
        for (int a = 0; a < 3; a++) {
            e[i] += fabsf(mp[a][i]) * amax[a];
        }
        e[i] *= 1.0f / 16384.0f;
    }
    const float xs = (xMode == GDX3DS_VTX_X_NONE) ? 1.0f : 1.2f;
    float ex = e[0] * xs + e[3];
    const float ey = e[1] + e[3];
    ex = ex > ey ? ex : ey;
    const float px = ex / wMin * 200.0f * 1000.0f;
    return px > 4.0e9f ? 4000000000u : static_cast<unsigned>(px);
}

} // namespace

void Interp::NrrCourseHotKey(void* out) const {
    CourseKey& k = *static_cast<CourseKey*>(out);
    k.combLo = static_cast<uint32_t>(mRdp.combine_mode);
    k.combHi = static_cast<uint32_t>(mRdp.combine_mode >> 32);
    k.oml = mRdp.other_mode_l;
    k.omh = mRdp.other_mode_h;
    k.geo = mRsp.geometry_mode;
    k.egeo = mRsp.extra_geometry_mode;
    k.prim = Rgba32(mRdp.prim_color);
    k.env = Rgba32(mRdp.env_color);
    k.fog = Rgba32(mRdp.fog_color);
    k.blend = Rgba32(mRdp.blend_color);
    k.primDepth = mRdp.prim_depth;
    k.variant = static_cast<uint32_t>(mVariant);
    k.curW = mCfg.curW;
    k.curH = mCfg.curH;
    k.wide = (mCfg.widescreen ? 1u : 0u) | (mCfg.forceFixedAspect ? 2u : 0u);
}

/* The memo of the current state (NULL: none, or its block went stale with the NRR state table). */
static const CourseMemo* NrrCourseHotMemo(const CourseKey& k) {
    const uint32_t gen = NrrStateGeneration();
    for (int i = 0; i < kMemos; i++) {
        const CourseMemo& m = sMemo[i];
        if (m.used && m.gen == gen && memcmp(&m.key, &k, sizeof(k)) == 0) {
            sMemo[i].lastUse = ++sMemoUse;
            return &m;
        }
    }
    return nullptr;
}

/* Cold: record the memo of a material right after its cold draw bound it (the backend's state is the
 * draw's): the core state key the machines' recorder builds (StageKey), its block, the inputs and the
 * uniform block. */
static GDX_COLD void NrrCourseRecord(const CourseKey& k, const Gdx3dsCourseCold& cold) {
    Gdx3dsCourseWin& w = gdx3ds_course_win;
    GdxNrrBackendRun b;
    memset(&b, 0, sizeof(b));
    if (!cold.memoOk || !gdx3ds_nrr_on() || !gdx3ds_nrr_backend_capture(&b) || !b.fogOk || b.tex[0] != nullptr ||
        b.tex[1] != nullptr) {
        w.memoFails++;
        return;
    }
    NrrStateKey sk;
    memset(&sk, 0, sizeof(sk));
    sk.shaderId0 = b.id0;
    sk.shaderId1 = b.id1;
    sk.tex[0] = sk.tex[1] = NRR_TEX_NONE;
    sk.program = NRR_PROG_XF;
    sk.depthTest = b.depthTest;
    sk.depthWrite = b.depthMask;
    sk.decal = b.decal;
    sk.blend = b.useAlpha ? NRR_BLEND_ALPHA : NRR_BLEND_OFF;
    sk.cull = cold.cullMode == GDX3DS_CULL_BACK ? NRR_CULL_BACK
                                                : (cold.cullMode == GDX3DS_CULL_FRONT ? NRR_CULL_FRONT : NRR_CULL_NONE);
    sk.alphaTest = NRR_AT_OFF; /* the shader's own edge / threshold flags decide (BuildEffect) */
    sk.alphaRef = b.alphaRef;
    CourseMemo* m = &sMemo[0];
    for (int i = 0; i < kMemos; i++) {
        if (!sMemo[i].used) {
            m = &sMemo[i];
            break;
        }
        if (sMemo[i].lastUse < m->lastUse) {
            m = &sMemo[i];
        }
    }
    if (m->mem == nullptr) {
        m->mem = static_cast<unsigned char*>(aligned_alloc(8, (NrrStateBytes() + 7u) & ~7u));
        if (m->mem == nullptr) {
            w.memoFails++;
            return;
        }
    }
    m->used = false;
    m->block = NrrStateBuildInto(&sk, m->mem);
    if (m->block == nullptr || NrrStateFogStage(m->block) >= 0) {
        w.memoFails++;
        return;
    }
    m->key = k;
    m->gen = NrrStateGeneration();
    memcpy(m->in, cold.in, sizeof(m->in));
    m->numInputs = cold.numInputs < 7 ? cold.numInputs : 7;
    memcpy(m->xf, cold.xf, sizeof(m->xf));
    m->xMode = cold.xMode;
    m->aspect = cold.aspect;
    m->lastUse = ++sMemoUse;
    m->used = true;
    w.memos++;
}

/* The hot draw of one model under a memo: 1 = taken. */
int Interp::NrrCourseHotDraw(const Gdx3dsCourseModel& m, const void* memoPtr, void* seamPtr) {
    const CourseMemo& mm = *static_cast<const CourseMemo*>(memoPtr);
    Gdx3dsCourseWin& w = gdx3ds_course_win;
    const float(*mp)[4] = mRsp.MP_matrix;
    float wMin = 0.0f;
    const int cover = NrrCourseHotCover(mp, mm, m, &wMin);
    if (cover == CV_OUTSIDE) {
        w.outside++; /* every triangle all-out: the CPU path binds nothing, draws nothing */
        return 1;
    }
    if (cover == CV_STRADDLE) {
        /* ni_course_clip used to gate this too (sibling getter call); deleted, W6 Fable review --
         * no documented use of the off path and no gate in this tree ever set it. */
        if (!(wMin > kCvMinW)) {
            w.fbClip++;
            return 0;
        }
        w.straddle++;
    }
    const unsigned err = NrrCourseHotErr(mp, mm.xMode, m, wMin);
    w.maxErrMpx = err > w.maxErrMpx ? err : w.maxErrMpx;
    const int maxErr = gdx3ds_mesh_course_maxerr();
    if (maxErr >= 0 && err > static_cast<unsigned>(maxErr)) {
        w.fbErr++;
        return 0;
    }
    ApplyViewportScissor(); /* this model's viewport and scissor, as its first stock resolve would */
    NrrCtx c;
    if (!gdx3ds_nrr_open_ext(&c, seamPtr, &sCourseStats, NRR_CAT_MACH)) {
        if (w.fbOpen == 0 && !gdx3ds_nrr_on() && &gdx_dbg_logf != nullptr) {
            /* One-time, not per-frame: [native] nrr=0 is the whole-game NRR master, and without it
             * every one of this and future frames' course draws takes this refusal and falls back
             * to the cold (round-1) draw -- correct, but it never engages the round-2 material memo.
             * Nothing else in the receipt says this plainly (`fb open=` just counts it). */
            gdx_dbg_logf("[nicourse] [native] nrr=0: course draws fall back to the cold (round-1) "
                         "path every frame -- set nrr=1 to engage the round-2 material memo\n");
        }
        w.fbOpen++;
        return 0;
    }
    c.keepVp = 1; /* the walk's viewport + scissor, the clip square cut (the CPU clipper's edge) */
    float xf[28][4];
    memcpy(xf, mm.xf, sizeof(xf));
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            xf[i][k] = mp[k][i];
        }
    }
    NrrStateApply(&c, mm.block);
    NrrSetInputs(&c, mm.block, mm.in, mm.numInputs);
    NrrDrawXf(&c, m.verts, 16, m.idx, m.nIdx, &xf[0][0]);
    gdx3ds_nrr_close(&c);
    w.drawn++;
    w.nrr++;
    w.tris += m.nIdx / 3u;
    return 1;
}

void Interp::NrrCourseHotMarker(uint32_t w0, uint32_t w1, void* seamPtr) {
    if ((w0 & 0x8000u) != 0 || mSp <= 0 || seamPtr == nullptr) {
        return; /* the second word: nothing (its run is being walked) */
    }
    Frame& f = mStack[mSp - 1];
    const uint32_t nCmds = w0 & 0x7FFFu;
    if (nCmds == 0 || f.list.isF3d || f.list.isBig || f.idx + 1u + nCmds > f.list.limit) {
        return;
    }
    const Cmd second = ReadCmd(f.list, f.idx);
    if (!IsCourseMarker(second) || (second.w0 & 0x8000u) == 0) {
        return;
    }
    const int level = gdx3ds_mesh_course_level();
    if (level != 1) {
        if (level == 2) {
            NrrCourseVerify(nCmds, w1, second, f, seamPtr); /* ni_course=2: the stock walk draws; the key is checked */
        }
        return;
    }
    const uint64_t t0 = svcGetSystemTick();
    Gdx3dsCourseRun run;
    run.words = f.list.base + (f.idx + 1u) * f.list.stride;
    run.stride = f.list.stride;
    run.nCmds = nCmds;
    run.vtxW1 = w1;
    run.vtxCount = second.w0 & 0x7FFFu;
    run.gen = second.w1;
    run.offscreen = 0;
    GdxInterpSeam* seam = static_cast<GdxInterpSeam*>(seamPtr);
    const Gdx3dsCourseModel* m = gdx3ds_mesh_course_model(*seam, run, GdxOwnedSeamResolveVtx, seam);
    int taken = 0;
    if (m != nullptr) {
        if (NrrCourseHotOffscreen()) {
            gdx3ds_course_win.offscreen++; /* whatever the CPU clipper keeps lies in a viewport no pixel of which is drawn */
            taken = 1;
        } else {
            CourseKey k;
            NrrCourseHotKey(&k);
            const CourseMemo* mm = NrrCourseHotMemo(k);
            taken = mm != nullptr ? NrrCourseHotDraw(*m, mm, seamPtr) : NrrCourseCold(*m, &k, seamPtr);
        }
    }
    gdx3ds_course_win.ticks += svcGetSystemTick() - t0;
    if (taken == 0) {
        return;
    }
    const size_t first = f.idx + 1u;
    f.idx = first + nCmds;
    if (!NrrCourseHotNextIsModel(f)) {
        NrrCourseLeave(f, first, m->tail);
    }
}

/* No memo for this state (its first frame, or the NRR hand-off unavailable): the round-1 draw, then
 * the memo is recorded from what it bound. */
GDX_COLD int Interp::NrrCourseCold(const Gdx3dsCourseModel& m, const void* keyPtr, void* seamPtr) {
    Gdx3dsCourseCold cold;
    memset(&cold, 0, sizeof(cold));
    const int taken = gdx3ds_mesh_course_cold(*static_cast<GdxInterpSeam*>(seamPtr), &m, &cold);
    if (taken != 0 && cold.drawn) {
        NrrCourseRecord(*static_cast<const CourseKey*>(keyPtr), cold);
    }
    return taken;
}

/* The model's viewport lies outside the scissor or the window: a stock walk of the run binds its state
 * and submits its triangles, and the clipper keeps no pixel of them. */
bool Interp::NrrCourseHotOffscreen() const {
    const Rect& v = mRdp.viewport;
    const Rect& s = mRdp.scissor;
    const int32_t vx0 = v.x;
    const int32_t vy0 = v.y;
    const int32_t vx1 = vx0 + static_cast<int32_t>(v.width);
    const int32_t vy1 = vy0 + static_cast<int32_t>(v.height);
    const int32_t sx0 = s.x;
    const int32_t sy0 = s.y;
    const int32_t sx1 = sx0 + static_cast<int32_t>(s.width);
    const int32_t sy1 = sy0 + static_cast<int32_t>(s.height);
    const int32_t ww = static_cast<int32_t>(mCfg.curW);
    const int32_t wh = static_cast<int32_t>(mCfg.curH);
    if (vx1 <= sx0 || vx0 >= sx1 || vy1 <= sy0 || vy0 >= sy1) {
        return true;
    }
    return vx1 <= 0 || vx0 >= ww || vy1 <= 0 || vy0 >= wh;
}

/* The last model of the frame: re-run its last G_VTX loads so the vertex cache holds what the stock
 * walk leaves (the second-to-last load fills the slots a short last load does not reach). */
GDX_COLD void Interp::NrrCourseLeave(Frame& f, size_t first, const uint32_t tail[2]) {
    Frame tf = f;
    for (int k = 0; k < 2; k++) {
        if (tail[k] == 0xFFFFFFFFu) {
            continue;
        }
        const size_t i = first + tail[k];
        const Cmd c = ReadCmd(f.list, i);
        tf.idx = i + 1u;
        if (GDX_UNLIKELY(mNc != nullptr)) {
            NcNote(c, false, NCS_COURSE); /* [debug] nicensus: an executed command */
        }
        (void)Exec(c, tf, 0);
    }
}

/* ni_course=2: nothing drawn here (the stock walk draws the run); the mesh module checks its baked copy
 * against the live words and vertex bytes -- the generation key's claim -- and counts mismatches. */
GDX_COLD void Interp::NrrCourseVerify(uint32_t nCmds, uint32_t w1, const Cmd& second, const Frame& f, void* seamPtr) {
    Gdx3dsCourseRun run;
    run.words = f.list.base + (f.idx + 1u) * f.list.stride;
    run.stride = f.list.stride;
    run.nCmds = nCmds;
    run.vtxW1 = w1;
    run.vtxCount = second.w0 & 0x7FFFu;
    run.gen = second.w1;
    run.offscreen = NrrCourseHotOffscreen() ? 1 : 0;
    gdx3ds_mesh_course_verify(run, GdxOwnedSeamResolveVtx, seamPtr);
}

} // namespace gdxn
