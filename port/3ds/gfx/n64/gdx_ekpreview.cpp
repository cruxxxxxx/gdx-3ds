/* port/3ds/gfx/n64/gdx_ekpreview.cpp -- ekeditmesh `[native] ek_editmesh` (o3dsek plan S5c): the
 * course editor's preview, its packed output retained per element. Expansion Kit builds only (the
 * whole TU is `#ifdef EXPANSION_KIT`, and CMake adds it to gdx3ds_n64 only under GDX_3DS_EXPANSION_KIT).
 *
 * WHAT IT REPLACES. The editor's 3D view is two static meshes the owned walker re-interprets every
 * frame:
 *   - the ground grid (course_edit/191080.c func_xk2_800E7028): a list built once of 30 x 30 textured
 *     quads, each  G_VTX(4 at slot 0)  G_TRI2;  900 VTX + 1800 triangles a task, ~85% of the
 *     editor's walk on the Old 3DS model (two T&Ls of 4, two SpTri: screen, ResolveDrawState, the
 *     packed emission with its per-vertex UV maths);
 *   - the course line (func_xk2_800E0988), once the course has 4+ control points: L3DEX2 wide lines,
 *     each  G_VTX(2 at slot 0)  G_RDPPIPESYNC  G_SETPRIMCOLOR  G_LINE3D(0, 1, width) (recorded and
 *     verified, but served only once a replay has proved it: kServePrimLines), and the selected
 *     segment's highlight (func_xk2_800E0BD0 / 800E17E0): G_VTX(2) G_LINE3D(0, 1, 10).
 *
 * THE MEMO. An element's packed output -- the triangles SpTri appends to the VBO batch -- is a
 * function of
 *   (a) its commands' words, the vertex bytes its G_VTX loads and (for a triangle element) the prim
 *       colour it shades with: the KEY. Content, not address, so the editor's double-buffered lists
 *       hit the same entry;
 *   (b) the context it is transformed and packed under (EkPvCtx: the MP matrix, geometry modes, the
 *       frame config, the combiner and blender registers, the clipper levers), compared whole at the
 *       start of every run of elements; up to kCtxSlots contexts live at once;
 *   (c) the bound draw state (EkPvDraw: combiner, shader, texture extents, the two tiles' UV
 *       parameters, the batch's pack parameters), proved again after the live ResolveDrawState a
 *       served run still performs.
 * A miss runs the stock commands and records what SpTri emitted (EkPvCaptureTri, at the emission
 * tail). A hit runs PIPESYNC/PRIM exactly as the walker would (so a prim change folds or flushes as
 * stock), ResolveDrawState once per run (idempotent between elements: nothing it reads changes
 * across VTX/PIPESYNC/PRIM/TRI, and Flush leaves mRs alone), then appends the recorded triangles with
 * SpTri's own batch protocol: open at mBufNumTris == 0 (VboPackBegin, a pure read), the aux latch
 * from the recorded vertex 0, the fog note, the cap flush. The packed floats are the bits stock
 * writes; only the CPU work behind them is gone.
 *
 * Refused (the element takes the stock path): replay / verify / census instrumentation, the HUD and
 * sky fast paths, lighting or texgen geometry, a clip fan of more than kTrisMax triangles, a batch the
 * backend refused while recording. When a run of served elements ends with anything but a G_VTX that
 * reloads the same slots, the last element's vertices are re-loaded by the stock T&L, so the vertex
 * cache the next command reads is the stock one.
 *
 * Memory: one linearAlloc'd table (~0.8 MiB) allocated at the first preview element and freed after
 * kIdleFree tasks without one, so the race (and the editor's test drive) never carries it.
 *
 * Lever: [native] ek_editmesh=1 (default 0); [native] ek_editmesh_verify=1 keeps the stock draw and
 * compares every cached element with a fresh recording. Receipt: `[ekpv] init` once, `[ekpv]
 * tasks=...` every kWindow tasks that drew an element, `[ekpv] alloc` / `free`. */
#ifdef EXPANSION_KIT

#include "gdx_interp.h"
#include "gdx_log.h"
#include "gdx3ds_hotcold.h"
#include "gdx3ds_replay.h" /* gdx3ds_replay_interp: the replay recorder is refused */

#include <3ds.h>
#include <string.h>

/* course_gadgets.c: the memo serves the course editor only (its preview, not the test drive and not
 * any other scene). Read on the drawing thread without a fence: a stale value draws the stock path
 * or one more memo frame of an unchanged editor list, both correct. */
extern "C" bool gInCourseEditor;
extern "C" bool gInCourseEditTestRun;

namespace gdxn {

namespace {
constexpr int kIndexSlots = 4096;   /* open-addressed index, a power of two */
constexpr int kEntries = 2048;      /* the grid's 900 quads + the course line's ~900 lines */
constexpr int kCtxSlots = 4;        /* live contexts (views) at once */
constexpr int kTrisMax = 2;         /* a quad or a line's quad; a clip fan beyond it stays stock */
constexpr int kKeyMax = 5 + 16;     /* 5 command / prim words + 4 vertices */
constexpr uint32_t kIdleFree = 120; /* tasks without a preview element before the table is released */
constexpr uint32_t kWindow = 64;    /* tasks with an element per receipt */
constexpr uint32_t kVfyLogMax = 8;
/* The coloured course line (func_xk2_800E0988, K_LINE below) has not been reached by any replay
 * yet: ek-editline's courses stay in the editor's invalid-course state, which draws the outline
 * with func_xk2_800E73DC (G_VTX of 1, never a candidate). Its elements are counted (primlines/t)
 * but stay stock until a replay proves them under ek_editmesh_verify. */
constexpr bool kServePrimLines = false;

enum : uint8_t { E_RECORD = 0, E_CACHED = 1, E_STOCK = 2 };
enum : uint8_t {
    K_DRAW = 1, /* G_VTX + the drawing command (TRI1/TRI2/QUAD, or LINE3D(0,1)): prim is live state */
    K_LINE = 2  /* G_VTX(2) + PIPESYNC + SETPRIMCOLOR + LINE3D(0,1): prim is in the element */
};

struct EkPvCtx {
    float mp[4][4];
    uint64_t combineMode;
    uint32_t geometryMode, extraGeometryMode, otherModeL, otherModeH;
    uint32_t curW, curH, clipPolyMax, vtxSoftCap;
    int32_t variant, o3dsFast;
    int16_t fogMul, fogOffset;
    uint16_t texS, texT;
    Rgba env, fog, blend, keyCenter, keyScale;
    int16_t convertK4, convertK5;
    uint8_t widescreen, forceFixedAspect, isBig, grayscale, skyNoClip, firstTile, pad[2];
};

struct EkPvDraw {
    const Combiner* comb;
    const void* prg;
    uint32_t tm;
    uint32_t texW[2], texH[2], texW2[2], texH2[2], effTile[2];
    Tile tiles[2];
    uint8_t numInputs, usedTex0, usedTex1, useAlpha, useFog, useBlend, zIsFrom0To1, invertY;
};
} // namespace

struct EkPvEntry {
    uint32_t key[kKeyMax];
    uint8_t keyLen, kind, state, nTris;
    uint16_t ctxSlot, ctxGen;
    Rgba v0Color[kTrisMax];
    float v0W[kTrisMax];
    float f[kTrisMax][3][GDX3DS_VBOPACK_STRIDE];
};

struct EkPvMemo {
    int16_t index[kIndexSlots];
    EkPvEntry entries[kEntries];
    int nEntries;
    EkPvCtx ctx[kCtxSlots];
    EkPvDraw draw[kCtxSlots];
    Gdx3dsVboPackParams params[kCtxSlots];
    uint8_t drawValid[kCtxSlots];
    uint16_t ctxGen[kCtxSlots];
    uint32_t ctxUse[kCtxSlots];
    uint8_t ctxLive[kCtxSlots];
    uint32_t useSeq;
    /* the run: elements back to back in one list, one context check and one resolve */
    const uint8_t* runBase;
    size_t runIdx;
    int runSlot;
    bool resolved;
    bool paramsOk;
    DrawState ds;
    /* the receipt window */
    uint32_t lineTasks, idle, seen;
    uint32_t quads, lines, plines, hits, misses, stock, recorded, refusedRec, rebind, mater, wipes, ctxNew, ctxRuns;
    uint32_t trisServed, fallback, paramsMid;
    uint32_t vfyOk, vfyBad, vfyLogged;
};

struct EkPvCap {
    EkPvEntry* e;
    int ctxSlot;
    bool bad;
    uint8_t n;
};

namespace {
/* why a candidate G_VTX did not take the memo (printed while no table is live) */
uint32_t sRefuse[4];

uint32_t KeyHash(const uint32_t* k, int len, uint32_t slot) {
    uint32_t h = 2166136261u ^ (slot * 0x9E3779B9u);
    for (int i = 0; i < len; i++) {
        h = (h ^ k[i]) * 16777619u;
    }
    return h ^ (h >> 15);
}

void MemoWipe(EkPvMemo* m) {
    memset(m->index, 0xFF, sizeof(m->index));
    m->nEntries = 0;
    m->resolved = false;
    m->runBase = nullptr;
    m->wipes++;
}

EkPvMemo* MemoAlloc() {
    EkPvMemo* m = static_cast<EkPvMemo*>(linearAlloc(sizeof(EkPvMemo)));
    if (m == nullptr) {
        GDXN_LOG("[ekpv] linearAlloc(%u) FAILED (linfree=%uK) -- the stock preview path",
                 static_cast<unsigned>(sizeof(EkPvMemo)), static_cast<unsigned>(linearSpaceFree() / 1024u));
        return nullptr;
    }
    memset(m, 0, sizeof(*m));
    MemoWipe(m);
    m->wipes = 0;
    GDXN_LOG("[ekpv] alloc %u bytes (entries=%d ctx=%d) linfree=%uK", static_cast<unsigned>(sizeof(EkPvMemo)),
             kEntries, kCtxSlots, static_cast<unsigned>(linearSpaceFree() / 1024u));
    return m;
}

void RunEnd(EkPvMemo* m, const uint8_t* base, size_t idx, int slot) {
    m->runBase = base;
    m->runIdx = idx; /* where the next element's G_VTX must sit for the run to continue */
    m->runSlot = slot;
}

void FillDraw(EkPvDraw* d, const DrawState& ds, const Rdp& rdp) {
    memset(d, 0, sizeof(*d));
    d->comb = ds.comb;
    d->prg = ds.prg;
    d->tm = ds.tm;
    for (int t = 0; t < 2; t++) {
        d->texW[t] = ds.tex_width[t];
        d->texH[t] = ds.tex_height[t];
        d->texW2[t] = ds.tex_width2[t];
        d->texH2[t] = ds.tex_height2[t];
        d->effTile[t] = ds.effective_tile[t];
        if (ds.usedTextures[t]) {
            memcpy(&d->tiles[t], &rdp.texture_tile[ds.effective_tile[t] & 7u], sizeof(Tile));
        }
    }
    d->numInputs = ds.numInputs;
    d->usedTex0 = ds.usedTextures[0] ? 1 : 0;
    d->usedTex1 = ds.usedTextures[1] ? 1 : 0;
    d->useAlpha = ds.use_alpha ? 1 : 0;
    d->useFog = ds.use_fog ? 1 : 0;
    d->useBlend = ds.use_blend_color ? 1 : 0;
    d->zIsFrom0To1 = ds.clip.zIsFrom0To1 ? 1 : 0;
    d->invertY = ds.clip.invertY ? 1 : 0;
}

bool DrawEq(const EkPvDraw& a, const DrawState& ds, const Rdp& rdp) {
    EkPvDraw b;
    FillDraw(&b, ds, rdp);
    return memcmp(&a, &b, sizeof(a)) == 0;
}
} // namespace

/* One command, exactly as Walk runs it (never an F3D list, and the walker's verify hook is refused,
 * so the read, the counters and Exec are all of it). */
void Interp::EkPvStep(const Cmd& c, Frame& f) {
    f.idx++;
    mStats.op[Opcode(c.w0)]++;
    mStats.cmds++;
    Exec(c, f, 0);
}

/* The stock element from its drawing command on: the G_VTX that brought us here, then the command
 * at f.idx (the TRI, or the LINE3D once PIPESYNC/PRIM ran), recorded into cap when non-null. */
void Interp::EkPvStock(const Cmd& vtx, Frame& f, int kind, EkPvCap* cap) {
    (void)kind;
    SpVertex(C0(vtx.w0, 12, 8), 0, vtx, f.list);
    const Cmd draw = ReadCmd(f.list, f.idx);
    if (cap == nullptr) {
        EkPvStep(draw, f);
        return;
    }
    const uint32_t refused0 = mStats.packRefused;
    cap->n = 0;
    cap->bad = false;
    mEkCap = cap;
    EkPvStep(draw, f);
    mEkCap = nullptr;
    if (mStats.packRefused != refused0) {
        cap->e->state = E_RECORD; /* the pool refused a batch: record it again next time */
        mEkPv->refusedRec++;
        return;
    }
    cap->e->nTris = cap->n;
    cap->e->state = cap->bad ? E_STOCK : E_CACHED;
    mEkPv->recorded++;
}

/* SpTri's emission tail while an element is being recorded: out = the three packed vertices. */
void Interp::EkPvCaptureTri(const float* out, const LoadedVertex* v0, const DrawState& ds) {
    EkPvCap* cap = mEkCap;
    if (cap->bad) {
        return;
    }
    EkPvMemo* m = mEkPv;
    const int s = cap->ctxSlot;
    if (cap->n >= kTrisMax || mHsxSkyVfy) {
        cap->bad = true;
        return;
    }
    if (!m->drawValid[s]) {
        FillDraw(&m->draw[s], ds, mRdp);
        memcpy(&m->params[s], &mPackParams, sizeof(mPackParams));
        m->drawValid[s] = 1;
    } else if (!DrawEq(m->draw[s], ds, mRdp) || memcmp(&m->params[s], &mPackParams, sizeof(mPackParams)) != 0) {
        cap->bad = true; /* one context, two draw states: this element stays stock */
        return;
    }
    EkPvEntry& e = *cap->e;
    const int t = cap->n++;
    memcpy(e.f[t], out, sizeof(e.f[t]));
    e.v0Color[t] = v0->color;
    e.v0W[t] = v0->w;
}

/* A cached element through SpTri's batch protocol. 1 = served, 0 = the bound draw no longer matches
 * the context's record and nothing was emitted (the caller runs the stock element). */
int Interp::EkPvServe(const EkPvEntry& e, int ctxSlot) {
    EkPvMemo* m = mEkPv;
    if (e.nTris == 0) {
        return 1; /* stock drops both triangles before the state block: nothing to bind or draw */
    }
    if (!m->resolved) {
        if (!ResolveDrawState(false, m->ds)) {
            return 1; /* stock's SpTri returns without drawing on the same failure */
        }
        m->rebind++;
        if (!m->drawValid[ctxSlot] || !DrawEq(m->draw[ctxSlot], m->ds, mRdp)) {
            return 0;
        }
        m->resolved = true;
        m->paramsOk = false;
    }
    const DrawState& ds = m->ds;
    for (int t = 0; t < e.nTris; t++) {
        mStats.tris++;
        if (mBufNumTris == 0) {
            uint32_t capTris = 0;
            mStats.triBegin++;
            mPackDst = mBackend->VboPackBegin(&mPackParams, &capTris);
            mPackBatch = mPackDst != nullptr;
            mPackComb = ds.comb;
            m->paramsOk = false;
            if (!mPackBatch) {
                mStats.packRefused++;
                mPackCapTris = static_cast<size_t>(kMaxTriBuffer);
            } else {
                mPackCapTris = capTris < static_cast<uint32_t>(kMaxTriBuffer) ? static_cast<size_t>(capTris)
                                                                             : static_cast<size_t>(kMaxTriBuffer);
                if (mPackCapTris < static_cast<size_t>(kMaxTriBuffer)) {
                    mStats.packSplit++;
                }
            }
        }
        if (!mPackBatch) {
            continue; /* stock drops this triangle the same way */
        }
        if (!m->paramsOk) {
            if (memcmp(&m->params[ctxSlot], &mPackParams, sizeof(mPackParams)) != 0) {
                if (t == 0) {
                    m->resolved = false;
                    return 0; /* an open but empty batch: stock's first SpTri reopens it */
                }
                m->paramsMid++; /* cannot happen (one shader, one texture state); counted, not hidden */
            }
            m->paramsOk = true;
        }
        if (mBufNumTris == 0) {
            LoadedVertex v0;
            memset(&v0, 0, sizeof(v0));
            v0.color = e.v0Color[t];
            v0.w = e.v0W[t];
            EkLatchAux(ds, v0);
        }
        memcpy(mPackDst + mBufNumTris * (3 * GDX3DS_VBOPACK_STRIDE), e.f[t], sizeof(e.f[t]));
        if (ds.use_fog) {
            if (ds.use_blend_color) {
                mBackend->FogNoteParams(0, 0, 0);
            } else {
                mBackend->FogNoteParams(mRsp.fog_mul, mRsp.fog_offset, 1);
            }
        }
        mStats.trisEmitted++;
        mStats.triEmitAll++;
        m->trisServed++;
        if (++mBufNumTris >= mPackCapTris) {
            Flush(FLUSH_FULL);
        }
    }
    return 1;
}

/* G_VTX. true = this VTX and the rest of its element were run here (f.idx past them). */
bool Interp::EkPvVtx(const Cmd& c, Frame& f) {
    if (!gInCourseEditor || gInCourseEditTestRun) {
        return false;
    }
    const uint32_t nVtx = C0(c.w0, 12, 8);
    if (nVtx < 2 || nVtx > 4 || C0(c.w0, 1, 7) != nVtx || f.list.isF3d || f.idx >= f.list.limit) {
        return false; /* not a preview element's load (slots 0..n-1, n <= 4) */
    }
    sRefuse[0]++;
    if (gdx3ds_replay_interp != nullptr || mVerifyActive || mNc != nullptr || mHudRectFast || mSkyRectFast ||
        mSkyPending || mHsxSkyVfy || mClipFanDepth != 0 || mVtxSoftCap < nVtx ||
        (mRsp.geometry_mode & (GM_LIGHTING | GM_TEXTURE_GEN)) != 0) {
        sRefuse[1]++;
        return false;
    }
    /* the element's shape */
    const Cmd nx = ReadCmd(f.list, f.idx);
    const uint8_t op = Opcode(nx.w0);
    uint8_t kind = 0;
    Cmd pr = { 0, 0, 0 };
    Cmd ln = { 0, 0, 0 };
    if (op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD) {
        bool fits = C0(nx.w0, 16, 8) / 2 < nVtx && C0(nx.w0, 8, 8) / 2 < nVtx && C0(nx.w0, 0, 8) / 2 < nVtx;
        if (op != OP_TRI1) {
            fits = fits && C1(nx.w1, 16, 8) / 2 < nVtx && C1(nx.w1, 8, 8) / 2 < nVtx && C1(nx.w1, 0, 8) / 2 < nVtx;
        }
        if (fits) {
            kind = K_DRAW;
        }
    } else if (op == OP_LINE3D && nVtx == 2 && mLineSection && C0(nx.w0, 16, 8) / 2 == 0 &&
               C0(nx.w0, 8, 8) / 2 == 1) {
        kind = K_DRAW; /* the selected segment's highlight (func_xk2_800E0BD0): no colour per line */
    } else if (op == OP_RDPPIPESYNC && nVtx == 2 && mLineSection && f.idx + 3 <= f.list.limit) {
        pr = ReadCmd(f.list, f.idx + 1);
        ln = ReadCmd(f.list, f.idx + 2);
        if (Opcode(pr.w0) == OP_SETPRIMCOLOR && Opcode(ln.w0) == OP_LINE3D && C0(ln.w0, 16, 8) / 2 == 0 &&
            C0(ln.w0, 8, 8) / 2 == 1) {
            kind = K_LINE;
            if (!kServePrimLines && mEkPvOn != 2) {
                if (mEkPv != nullptr) {
                    mEkPv->plines++;
                }
                return false;
            }
        }
    }
    if (kind == 0) {
        sRefuse[2]++;
        return false;
    }
    const uintptr_t p =
        mSrc.ResolveData(c, nVtx * 16u, /*preferPhysical=*/true, reinterpret_cast<uintptr_t>(f.list.base));
    if (p == 0 || DlSrc::ReadableBytes(p) < nVtx * 16u) {
        sRefuse[3]++;
        return false;
    }
    if (mEkPv == nullptr) {
        mEkPv = MemoAlloc();
        if (mEkPv == nullptr) {
            mEkPvOn = 0;
            return false;
        }
    }
    EkPvMemo* m = mEkPv;
    m->seen++;
    if (kind == K_LINE) {
        m->plines++;
    } else if (op == OP_LINE3D) {
        m->lines++;
    } else {
        m->quads++;
    }

    /* the context: compared whole at the start of a run; inside one, only element commands ran */
    int slot = -1;
    if (m->runBase == f.list.base && m->runIdx == f.idx - 1) {
        slot = m->runSlot;
    } else {
        m->resolved = false;
        m->ctxRuns++;
        EkPvCtx cx;
        memset(&cx, 0, sizeof(cx));
        memcpy(cx.mp, mRsp.MP_matrix, sizeof(cx.mp));
        cx.combineMode = mRdp.combine_mode;
        cx.geometryMode = mRsp.geometry_mode;
        cx.extraGeometryMode = mRsp.extra_geometry_mode;
        cx.otherModeL = mRdp.other_mode_l;
        cx.otherModeH = mRdp.other_mode_h;
        cx.curW = mCfg.curW;
        cx.curH = mCfg.curH;
        cx.clipPolyMax = static_cast<uint32_t>(mClipPolyMax);
        cx.vtxSoftCap = mVtxSoftCap;
        cx.variant = mVariant;
        cx.o3dsFast = mO3dsFast;
        cx.fogMul = mRsp.fog_mul;
        cx.fogOffset = mRsp.fog_offset;
        cx.texS = mRsp.texture_scaling_factor.s;
        cx.texT = mRsp.texture_scaling_factor.t;
        cx.env = mRdp.env_color;
        cx.fog = mRdp.fog_color;
        cx.blend = mRdp.blend_color;
        cx.keyCenter = mRdp.key_center;
        cx.keyScale = mRdp.key_scale;
        cx.convertK4 = mRdp.convert_k[4];
        cx.convertK5 = mRdp.convert_k[5];
        cx.widescreen = mCfg.widescreen ? 1 : 0;
        cx.forceFixedAspect = mCfg.forceFixedAspect ? 1 : 0;
        cx.isBig = f.list.isBig ? 1 : 0;
        cx.grayscale = mRdp.grayscale ? 1 : 0;
        cx.skyNoClip = mSkyNoClip ? 1 : 0;
        cx.firstTile = mRdp.first_tile_index;
        for (int s = 0; s < kCtxSlots; s++) {
            if (m->ctxLive[s] && memcmp(&m->ctx[s], &cx, sizeof(cx)) == 0) {
                slot = s;
                break;
            }
        }
        if (slot < 0) {
            slot = 0;
            for (int s = 1; s < kCtxSlots && m->ctxLive[slot]; s++) {
                if (!m->ctxLive[s] || m->ctxUse[s] < m->ctxUse[slot]) {
                    slot = s;
                }
            }
            m->ctx[slot] = cx;
            m->ctxLive[slot] = 1;
            m->drawValid[slot] = 0;
            if (++m->ctxGen[slot] == 0) {
                MemoWipe(m); /* the generation wrapped: no stale entry may alias a live one */
                m->ctxGen[slot] = 1;
            }
            m->ctxNew++;
        }
        m->ctxUse[slot] = ++m->useSeq;
    }

    /* the element's key */
    uint32_t key[kKeyMax];
    int len = 0;
    key[len++] = c.w0;
    if (kind == K_DRAW) {
        uint32_t prim = 0;
        memcpy(&prim, &mRdp.prim_color, sizeof(prim));
        key[len++] = nx.w0;
        key[len++] = nx.w1;
        key[len++] = prim;
        key[len++] = mRdp.prim_lod_fraction;
    } else {
        key[len++] = pr.w0;
        key[len++] = pr.w1;
        key[len++] = ln.w0;
        key[len++] = ln.w1;
    }
    memcpy(&key[len], reinterpret_cast<const void*>(p), nVtx * 16u);
    len += static_cast<int>(nVtx * 4u);
    const uint32_t h = KeyHash(key, len, static_cast<uint32_t>(slot) | (static_cast<uint32_t>(kind) << 8));
    EkPvEntry* e = nullptr;
    int at = static_cast<int>(h & (kIndexSlots - 1));
    for (int probe = 0; probe < kIndexSlots; probe++, at = (at + 1) & (kIndexSlots - 1)) {
        const int16_t ix = m->index[at];
        if (ix < 0) {
            break;
        }
        EkPvEntry& c2 = m->entries[ix];
        if (c2.ctxSlot == slot && c2.ctxGen == m->ctxGen[slot] && c2.kind == kind && c2.keyLen == len &&
            memcmp(c2.key, key, static_cast<size_t>(len) * 4u) == 0) {
            e = &c2;
            break;
        }
    }
    if (e == nullptr) {
        if (m->nEntries >= kEntries) {
            MemoWipe(m);
            at = static_cast<int>(h & (kIndexSlots - 1));
        }
        while (m->index[at] >= 0) {
            at = (at + 1) & (kIndexSlots - 1);
        }
        m->index[at] = static_cast<int16_t>(m->nEntries);
        e = &m->entries[m->nEntries++];
        memset(e, 0, sizeof(*e));
        memcpy(e->key, key, static_cast<size_t>(len) * 4u);
        e->keyLen = static_cast<uint8_t>(len);
        e->kind = kind;
        e->ctxSlot = static_cast<uint16_t>(slot);
        e->ctxGen = m->ctxGen[slot];
        e->state = E_RECORD;
    }

    /* the commands before the drawing one (a line's PIPESYNC/PRIM) run as the walker runs them */
    if (kind == K_LINE) {
        EkPvStep(nx, f);
        EkPvStep(pr, f);
    }
    EkPvCap cap = { nullptr, slot, false, 0 };
    if (e->state != E_CACHED || mEkPvOn == 2) {
        if (e->state == E_STOCK) {
            m->stock++;
            EkPvStock(c, f, kind, nullptr);
        } else if (e->state == E_RECORD) {
            m->misses++;
            cap.e = e;
            EkPvStock(c, f, kind, &cap);
        } else {
            /* verify: the stock element, recorded into a scratch copy and compared with the memo */
            EkPvEntry fresh = *e;
            cap.e = &fresh;
            EkPvStock(c, f, kind, &cap);
            const size_t nt = e->nTris;
            const bool same = fresh.state == E_CACHED && fresh.nTris == e->nTris &&
                              memcmp(fresh.f, e->f, sizeof(e->f[0]) * nt) == 0 &&
                              memcmp(fresh.v0Color, e->v0Color, sizeof(Rgba) * nt) == 0 &&
                              memcmp(fresh.v0W, e->v0W, sizeof(float) * nt) == 0;
            if (same) {
                m->vfyOk++;
            } else {
                m->vfyBad++;
                if (m->vfyLogged < kVfyLogMax) {
                    m->vfyLogged++;
                    GDXN_LOG("[ekpv] VERIFY MISMATCH kind=%u key=%08x/%08x/%08x tris=%u/%u state=%u ctx=%d",
                             static_cast<unsigned>(kind), key[0], key[1], key[5], static_cast<unsigned>(e->nTris),
                             static_cast<unsigned>(fresh.nTris), static_cast<unsigned>(fresh.state), slot);
                }
            }
        }
        m->resolved = false; /* the stock SpTri resolved; the next served element proves its own */
        RunEnd(m, f.list.base, f.idx, slot);
        return true;
    }

    /* served */
    const Cmd drawCmd = ReadCmd(f.list, f.idx);
    f.idx++;
    mStats.op[Opcode(drawCmd.w0)]++;
    mStats.cmds++;
    if (EkPvServe(*e, slot) == 0) {
        /* the bound draw moved under the context: retire the context's records, draw stock, record */
        m->fallback++;
        m->drawValid[slot] = 0;
        if (++m->ctxGen[slot] == 0) {
            MemoWipe(m);
            m->ctxGen[slot] = 1;
        }
        m->resolved = false;
        f.idx--;
        mStats.op[Opcode(drawCmd.w0)]--;
        mStats.cmds--;
        e->ctxGen = m->ctxGen[slot];
        e->state = E_RECORD;
        cap.e = e;
        EkPvStock(c, f, kind, &cap);
        RunEnd(m, f.list.base, f.idx, slot);
        return true;
    }
    m->hits++;
    RunEnd(m, f.list.base, f.idx, slot);
    /* the vertex cache the next command reads: slots 0..n-1 as the stock T&L leaves them (a
     * following G_VTX that reloads at least those slots from slot 0 makes that moot) */
    bool nextReloads = false;
    if (f.idx < f.list.limit) {
        const uint32_t w0 = ReadCmd(f.list, f.idx).w0;
        const uint32_t n = C0(w0, 12, 8);
        nextReloads = Opcode(w0) == OP_VTX && n >= nVtx && C0(w0, 1, 7) == n;
    }
    if (!nextReloads) {
        m->mater++;
        SpVertex(nVtx, 0, c, f.list);
    }
    return true;
}

void Interp::EkPvTaskEnd() {
    EkPvMemo* m = mEkPv;
    static bool sInitLogged = false;
    static uint32_t sNoTable = 0;
    if (!sInitLogged) {
        sInitLogged = true;
        GDXN_LOG("[ekpv] init ek_editmesh=%d (%s) entries=%d ctx=%d tris_max=%d bytes=%u", mEkPvOn,
                 mEkPvOn == 2 ? "verify" : (mEkPvOn == 1 ? "serve" : "off"), kEntries, kCtxSlots, kTrisMax,
                 static_cast<unsigned>(sizeof(EkPvMemo)));
    }
    if (m == nullptr) {
        if (sRefuse[0] != 0 && ++sNoTable >= kWindow) {
            GDXN_LOG("[ekpv] no table: candidates=%u refused instr=%u shape=%u src=%u (%u tasks)", sRefuse[0],
                     sRefuse[1], sRefuse[2], sRefuse[3], sNoTable);
            memset(sRefuse, 0, sizeof(sRefuse));
            sNoTable = 0;
        }
        return;
    }
    m->resolved = false; /* a run never spans tasks */
    m->runBase = nullptr;
    if (m->seen == 0) {
        if (++m->idle >= kIdleFree) {
            GDXN_LOG("[ekpv] free after %u idle tasks (entries=%d) linfree=%uK", m->idle, m->nEntries,
                     static_cast<unsigned>(linearSpaceFree() / 1024u));
            linearFree(m);
            mEkPv = nullptr;
        }
        return;
    }
    m->idle = 0;
    m->seen = 0;
    if (++m->lineTasks >= kWindow) {
        const uint32_t n = m->lineTasks;
        GDXN_LOG("[ekpv] tasks=%u quads/t=%u lines/t=%u primlines/t=%u hit/t=%u miss/t=%u stock/t=%u runs/t=%u rebind/t=%u "
                 "mater/t=%u tris/t=%u | rec=%u recrefused=%u fallback=%u paramsmid=%u entries=%d wipes=%u "
                 "ctxnew=%u | refused instr=%u shape=%u src=%u | vfy ok=%u bad=%u",
                 n, m->quads / n, m->lines / n, m->plines / n, m->hits / n, m->misses / n, m->stock / n, m->ctxRuns / n,
                 m->rebind / n, m->mater / n, m->trisServed / n, m->recorded, m->refusedRec, m->fallback,
                 m->paramsMid, m->nEntries, m->wipes, m->ctxNew, sRefuse[1], sRefuse[2], sRefuse[3], m->vfyOk,
                 m->vfyBad);
        memset(sRefuse, 0, sizeof(sRefuse));
        m->lineTasks = 0;
        m->quads = m->lines = m->plines = m->hits = m->misses = m->stock = m->ctxRuns = 0;
        m->recorded = m->refusedRec = m->rebind = m->mater = m->wipes = m->ctxNew = m->trisServed = 0;
        m->fallback = m->paramsMid = 0;
        m->vfyOk = m->vfyBad = 0;
    }
}

} // namespace gdxn

#endif /* EXPANSION_KIT */
