/* port/3ds/gfx/nrr/gdx3ds_nrr.cpp -- NRR core, render half: the 0x63 marker dispatch, the
 * hand-off with the owned interpreter's backend, the state-block table and its hot apply, the
 * streaming ring, the draw calls and the [nrr] receipts. Contract: gdx3ds_nrr.h.
 *
 * Two emitters sit behind the same API ([native] nrr_direct, gdx3ds_nrr_core.h): the citro3d path
 * (0: state into citro3d's context, C3D_DrawArrays / C3D_DrawElements emit it -- NrrCoreC3d*) and
 * the direct path (1: precomputed command words straight into the command buffer --
 * gdx3ds_nrr_direct.cpp, NrrCoreDx*); 2 runs the citro3d path and checks the direct words against
 * it (gdx3ds_nrr_dxverify.cpp).
 *
 * HOT CODE (counted by tools/native/nrr_size.sh against design 1.1, "core 2.0 KB"): every function
 * named NrrCoreHot* plus the public hot entries NrrStateApply, NrrSetInputs, NrrRingAlloc,
 * NrrDraw2D, NrrDrawXf, gdx3ds_nrr_marker, gdx3ds_nrr_draw, NrrBegin, NrrEnd, and per emitter
 * NrrCoreC3d* or NrrCoreDx*. Everything else here is cold (first sight, receipts) and marked
 * GDX_COLD. */
#include "gdx3ds_nrr_core.h"

#include <cstdio>
#include <cstring>

#include "../gdx3ds_hotcold.h"
#include "../gdx3ds_interp_seam.h"
#include "../gdx3ds_vbopack.h"
#include "gdx3ds_nrr_crumb.h" /* n60hwdiag: [debug] nrr_crumbs */
#include "../gdx3ds_skylate.h" /* n60tail: [native] sky_late watches for sky hand-offs inside the track run */

extern "C" int gdx3ds_c5cat_armed(void) __attribute__((weak));
extern "C" long long gdx3ds_prof_now(void) __attribute__((weak));

using namespace nrrcore;

NrrBkInfo nrrcore::gBk = {};
int nrrcore::gDirect = 0;

namespace {

constexpr int kWindow = 64;

NrrDrawFn sDraw[NRR_CAT_COUNT];
NrrPeekFn sPeek[NRR_CAT_COUNT]; /* n60trkv: hand-off-free markers (gdx3ds_nrr_set_peek) */
NrrCatStats sStats[NRR_CAT_COUNT];
uint32_t sTasks = 0;
unsigned long sTaskTotal = 0;
uint32_t sStateMisses = 0;
uint32_t sStateFull = 0;
uint32_t sRingOut = 0;
uint32_t sBadMarkers = 0;

NrrState sStates[NRR_STATE_MAX];
int sStateCount = 0;
uint32_t sStateGen = 1;

bool sBkValid = false;

/* Per hand-off: the texture copies NRR binds (reset by NrrBegin). */
C3D_Tex sTexCopy[2];
const void* sTexSrc[2];

struct NrrSaved {
    C3D_AttrInfo attr;
    C3D_BufInfo buf;
    C3D_Effect effect;
    u32 viewport[5];
    u32 scissor[3];
    C3D_TexEnv texEnv[kStages];
    u32 texEnvBuf;
    C3D_Tex* tex[3];
    C3D_FVec proj[4];
    C3D_FVec xf[kXfVec4s];
    C3D_FVec stk;
    u16 bools;
};
NrrSaved sSaved;

bool ClockOn() {
    return &gdx3ds_c5cat_armed != nullptr && &gdx3ds_prof_now != nullptr && gdx3ds_c5cat_armed() != 0;
}

GDX_COLD bool BackendLatch() {
    if (!sBkValid) {
        sBkValid = gdx3ds_nrr_bk_info(&gBk) != 0;
        if (sBkValid) {
            gDirect = gdx3ds_nrr_direct_level();
            NrrDxLatch();
        }
    }
    return sBkValid;
}

/* ------------------------------------------------- n60hwdiag: durable crumbs (cold) ---- */

constexpr int kCrumbHandoffs = 8; /* NrrBegin/NrrEnd pairs per category per epoch */
bool sCrumbOpen[NRR_CAT_COUNT];   /* this hand-off's begin was crumbed: crumb its end too */

GDX_COLD void CrumbHandoffBegin(int cat, int view, const NrrFrame* f) {
    gdx3ds_nrr_crumb_set_cat(cat);
    sCrumbOpen[cat] = gdx3ds_nrr_crumb_want(NRR_CRUMB_HANDOFF + cat, kCrumbHandoffs) != 0;
    if (sCrumbOpen[cat]) {
        gdx3ds_nrr_crumb("handoff > NrrBegin cat=%s view=%d frame=%p tick=%u direct=%d cmdbuf off=%lu/%lu",
                         gdx3ds_nrr_cat_name(cat), view, (const void*)f, NrrFrameTick(f), gDirect,
                         (unsigned long)gpuCmdBufOffset, (unsigned long)gpuCmdBufSize);
    }
}

GDX_COLD void CrumbHandoffEnd(int cat, int rc, uint32_t saved) {
    if (sCrumbOpen[cat]) {
        sCrumbOpen[cat] = false;
        gdx3ds_nrr_crumb("handoff < NrrEnd cat=%s rc=%d saved=0x%03lx cmdbuf off=%lu/%lu", gdx3ds_nrr_cat_name(cat), rc,
                         (unsigned long)saved, (unsigned long)gpuCmdBufOffset, (unsigned long)gpuCmdBufSize);
    }
    gdx3ds_nrr_crumb_set_cat(-1);
}

GDX_COLD void CrumbMarker(int cat, int view, const NrrFrame* f) {
    NRR_CRUMB_FIRST(NRR_CRUMB_MARKER + cat, 2, "marker 0x63 seen cat=%s view=%d frame=%p used=%lu ovf=%lu",
                    gdx3ds_nrr_cat_name(cat), view, (const void*)f, (unsigned long)f->used, (unsigned long)f->ovf);
}

/* The texture a state block binds on `unit`: its C3D_Tex and where its data is. */
GDX_COLD int CrumbTexOf(char* out, int cap, const NrrState* s, int unit) {
    const C3D_Tex* t = s != nullptr ? static_cast<const C3D_Tex*>(s->tex[unit].obj) : nullptr;
    NrrBkTex bt;
    if (t == nullptr && s != nullptr && s->tex[unit].id != NRR_TEX_NONE && gdx3ds_nrr_bk_tex(s->tex[unit].id, &bt)) {
        t = static_cast<const C3D_Tex*>(bt.tex);
    }
    if (t == nullptr) {
        return snprintf(out, (size_t)cap, " t%d=none", unit);
    }
    char b[96];
    gdx3ds_nrr_crumb_buf(b, sizeof(b), "data", t->data);
    return snprintf(out, (size_t)cap, " t%d=%ux%u fmt=%d size=%lu %s", unit, (unsigned)t->width, (unsigned)t->height,
                    (int)t->fmt, (unsigned long)t->size, b);
}

GDX_COLD void CrumbState(const NrrStateKey* k, const NrrState* s, bool into) {
    const int cat = gdx3ds_nrr_crumb_cat();
    if (!gdx3ds_nrr_crumb_want(NRR_CRUMB_STATE + (cat >= 0 ? cat : NRR_CAT_SELFTEST), 6)) {
        return;
    }
    char t0[160];
    char t1[160];
    CrumbTexOf(t0, sizeof(t0), s, 0);
    CrumbTexOf(t1, sizeof(t1), s, 1);
    gdx3ds_nrr_crumb("state build%s cat=%s prog=%d sh=%016llx/%016llx -> %s n=%d%s%s", into ? " (into)" : "",
                     cat >= 0 ? gdx3ds_nrr_cat_name(cat) : "none", (int)k->program, (unsigned long long)k->shaderId0,
                     (unsigned long long)k->shaderId1, s != nullptr ? "ok" : "FAILED", sStateCount, t0, t1);
}

GDX_COLD void CrumbRing(const NrrCtx* c, uint32_t bytes, const void* p) {
    if (!gdx3ds_nrr_crumb_want(NRR_CRUMB_RING + c->cat, 2)) {
        return;
    }
    char b[96];
    gdx3ds_nrr_crumb_buf(b, sizeof(b), "p", p);
    gdx3ds_nrr_crumb("ring alloc cat=%s bytes=%lu %s", gdx3ds_nrr_cat_name(c->cat), (unsigned long)bytes, b);
}

GDX_COLD void CrumbDraw(const NrrCtx* c, bool xf, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t n) {
    if (!gdx3ds_nrr_crumb_want((xf ? NRR_CRUMB_DRAWXF : NRR_CRUMB_DRAW2D) + c->cat, 4)) {
        return;
    }
    char vb[96];
    char ib[96];
    char t0[160];
    char t1[160];
    gdx3ds_nrr_crumb_buf(vb, sizeof(vb), "verts", verts);
    if (xf) {
        gdx3ds_nrr_crumb_buf(ib, sizeof(ib), "idx", idx);
    } else {
        ib[0] = '\0';
    }
    CrumbTexOf(t0, sizeof(t0), c->last, 0);
    CrumbTexOf(t1, sizeof(t1), c->last, 1);
    gdx3ds_nrr_crumb("draw %s > cat=%s view=%d n=%lu stride=%lu %s %s keepVp=%d direct=%d cmdbuf off=%lu/%lu%s%s",
                     xf ? "Xf" : "2D", gdx3ds_nrr_cat_name(c->cat), c->view, (unsigned long)n, (unsigned long)stride, vb,
                     ib, (int)c->keepVp, gDirect, (unsigned long)gpuCmdBufOffset, (unsigned long)gpuCmdBufSize, t0, t1);
}

GDX_COLD void CrumbFallback(int cat, int view, int rc) {
    NRR_CRUMB_FIRST(NRR_CRUMB_FALLBACK + cat, 4, "fallback cat=%s view=%d rc=%d", gdx3ds_nrr_cat_name(cat), view, rc);
}

/* ------------------------------------------------------------- state: cold build ---- */

GPU_TEXTURE_WRAP_PARAM WrapFromN64(uint8_t cm) {
    /* G_TX_MIRROR = 1, G_TX_CLAMP = 2 (gfx_citro3d.cpp WrapFromN64) */
    if ((cm & 2u) != 0) {
        return GPU_CLAMP_TO_EDGE;
    }
    if ((cm & 1u) != 0) {
        return GPU_MIRRORED_REPEAT;
    }
    return GPU_REPEAT;
}

/* The backend's ApplyShaderState for one stage, into a block-owned C3D_TexEnv. */
GDX_COLD void BuildTexEnv(NrrState& s) {
    s.numEnv = s.sh.numStages;
    for (int i = 0; i < kStages; i++) {
        C3D_TexEnv* env = &s.env[i];
        C3D_TexEnvInit(env);
        if (i >= s.sh.numStages) {
            continue;
        }
        const NrrBkStage& st = s.sh.stages[i];
        C3D_TexEnvFunc(env, C3D_RGB, (GPU_COMBINEFUNC)st.rgbFunc);
        C3D_TexEnvFunc(env, C3D_Alpha, (GPU_COMBINEFUNC)st.alphaFunc);
        C3D_TexEnvSrc(env, C3D_RGB, (GPU_TEVSRC)st.rgbSrc[0], (GPU_TEVSRC)st.rgbSrc[1], (GPU_TEVSRC)st.rgbSrc[2]);
        C3D_TexEnvSrc(env, C3D_Alpha, (GPU_TEVSRC)st.alphaSrc[0], (GPU_TEVSRC)st.alphaSrc[1],
                      (GPU_TEVSRC)st.alphaSrc[2]);
        C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)st.rgbOp[0], (GPU_TEVOP_RGB)st.rgbOp[1], (GPU_TEVOP_RGB)st.rgbOp[2]);
        C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)st.alphaOp[0], (GPU_TEVOP_A)st.alphaOp[1], (GPU_TEVOP_A)st.alphaOp[2]);
        C3D_TexEnvColor(env, st.constColor);
        if (st.constRgbInput != 0 || st.constAlphaInput != 0) {
            NrrConstSrc& cs = s.cs[s.numConst++];
            cs.stage = (uint8_t)i;
            cs.rgbIn = st.constRgbInput > 0 ? (uint8_t)st.constRgbInput : 0;
            cs.aIn = (st.constAlphaInput > 0 && s.sh.inputSize == 4) ? (uint8_t)st.constAlphaInput : 0;
            cs.rgbOne = st.constRgbFixed == 0 ? 0 : 1;
            cs.aOne = st.constAlphaFixed == 0 ? 0 : 1;
        }
    }
}

/* The effect and scissor words, produced by the real setters on citro3d's context and copied out;
 * the context (and its dirty flags) is put back exactly. Reversed depth (near = 1): N64 LEQUAL is
 * GEQUAL, a write without a test keeps the unit on with an always-pass function. */
GDX_COLD void BuildEffect(NrrState& s, const NrrStateKey& k) {
    C3D_Context* ctx = C3Di_GetContext();
    const C3D_Effect savedEffect = ctx->effect;
    u32 savedScissor[3];
    std::memcpy(savedScissor, ctx->scissor, sizeof(savedScissor));
    const u32 savedFlags = ctx->flags;
    C3D_DepthTest(k.depthTest || k.depthWrite, k.depthTest ? GPU_GEQUAL : GPU_ALWAYS,
                  k.depthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    gdx3ds_nrr_bk_depth_map(k.decal); /* the backend's decal bias */
    if (k.blend == NRR_BLEND_ALPHA) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE,
                       GPU_ONE_MINUS_SRC_ALPHA);
    } else {
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
    }
    if (k.alphaTest == NRR_AT_EDGE || s.sh.texEdge) {
        C3D_AlphaTest(true, GPU_GEQUAL, 128);
    } else if (k.alphaTest == NRR_AT_THRESHOLD || s.sh.alphaThreshold) {
        C3D_AlphaTest(true, GPU_GEQUAL, k.alphaRef);
    } else {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    }
    C3D_CullFace(k.cull == NRR_CULL_BACK ? GPU_CULL_BACK_CCW
                                         : (k.cull == NRR_CULL_FRONT ? GPU_CULL_FRONT_CCW : GPU_CULL_NONE));
    if (k.scissor[0] == 0 && k.scissor[1] == 0 && k.scissor[2] == 0 && k.scissor[3] == 0) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    } else {
        gdx3ds_nrr_bk_scissor(k.scissor[0], k.scissor[1], k.scissor[2], k.scissor[3]);
    }
    s.effect = ctx->effect;
    std::memcpy(s.scissor, ctx->scissor, sizeof(s.scissor));
    ctx->effect = savedEffect;
    std::memcpy(ctx->scissor, savedScissor, sizeof(savedScissor));
    ctx->flags = savedFlags;
}

/* The C3D_Tex param bits of a filter + wrap, as C3D_TexSetFilter / C3D_TexSetWrap set them. */
GDX_COLD void BuildTexUnit(NrrTexUnit& t, uint32_t id, bool linear, GPU_TEXTURE_WRAP_PARAM s, GPU_TEXTURE_WRAP_PARAM tw) {
    C3D_Tex probe;
    std::memset(&probe, 0, sizeof(probe));
    probe.param = 0xFFFFFFFFu;
    C3D_TexSetFilter(&probe, GPU_NEAREST, GPU_NEAREST); /* NEAREST and CLAMP_TO_EDGE are 0: */
    C3D_TexSetWrap(&probe, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE); /* what stays set is ~mask */
    t.mask = ~probe.param;
    probe.param = 0;
    const GPU_TEXTURE_FILTER_PARAM f = linear ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(&probe, f, f);
    C3D_TexSetWrap(&probe, s, tw);
    t.bits = probe.param;
    t.id = id;
}

GDX_COLD void BuildTextures(NrrState& s, const NrrStateKey& k) {
    for (int u = 0; u < 2; u++) {
        const uint32_t id = s.sh.usedTex[u] ? k.tex[u] : NRR_TEX_NONE;
        const GPU_TEXTURE_WRAP_PARAM ws = s.sh.clamp[u][0] ? GPU_CLAMP_TO_EDGE : WrapFromN64(k.texCms[u]);
        const GPU_TEXTURE_WRAP_PARAM wt = s.sh.clamp[u][1] ? GPU_CLAMP_TO_EDGE : WrapFromN64(k.texCmt[u]);
        BuildTexUnit(s.tex[u], id, k.texLinear[u] != 0, ws, wt);
        s.tex[u].obj = s.sh.usedTex[u] ? k.texObj[u] : nullptr;
    }
}

GDX_COLD bool StateFill(NrrState& s, const NrrStateKey* k) {
    std::memset(&s, 0, sizeof(s));
    s.key = *k;
    s.gen = sStateGen;
    if (!gdx3ds_nrr_bk_translate(k->shaderId0, k->shaderId1, &s.sh)) {
        return false;
    }
    BuildTexEnv(s);
    BuildEffect(s, *k);
    BuildTextures(s, *k);
    NrrDxBuildState(s);
    return true;
}

GDX_COLD const NrrState* StateBuild(const NrrStateKey* k) {
    sStateMisses++;
    if (sStateCount >= NRR_STATE_MAX) {
        sStateFull++;
        return nullptr;
    }
    NrrState& s = sStates[sStateCount];
    const bool ok = StateFill(s, k);
    if (ok) {
        sStateCount++;
    }
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbState(k, ok ? &s : nullptr, false);
    }
    return ok ? &s : nullptr;
}

/* ------------------------------------------------------------- the hand-off ---- */

void NrrCoreC3dSaveGroup(NrrCtx* c, uint32_t g) {
    C3D_Context* ctx = C3Di_GetContext();
    switch (g) {
        case G_ATTR: sSaved.attr = ctx->attrInfo; break;
        case G_BUF: sSaved.buf = ctx->bufInfo; break;
        case G_EFFECT: sSaved.effect = ctx->effect; break;
        case G_VP:
            std::memcpy(sSaved.viewport, ctx->viewport, sizeof(sSaved.viewport));
            std::memcpy(sSaved.scissor, ctx->scissor, sizeof(sSaved.scissor));
            break;
        case G_TEXENV: std::memcpy(sSaved.texEnv, ctx->texEnv, sizeof(sSaved.texEnv)); break;
        case G_ENVBUF: sSaved.texEnvBuf = ctx->texEnvBuf; break;
        case G_TEX: std::memcpy(sSaved.tex, ctx->tex, sizeof(sSaved.tex)); break;
        case G_PROJ: std::memcpy(sSaved.proj, &C3D_FVUnif[GPU_VERTEX_SHADER][gBk.projLoc], sizeof(sSaved.proj)); break;
        case G_XF: std::memcpy(sSaved.xf, &C3D_FVUnif[GPU_VERTEX_SHADER][gBk.xfUnifLoc], sizeof(sSaved.xf)); break;
        case G_BOOL: sSaved.bools = C3D_BoolUnifs[GPU_VERTEX_SHADER]; break;
        case G_STK: sSaved.stk = C3D_FVUnif[GPU_VERTEX_SHADER][gBk.stkLoc]; break;
        default: break;
    }
    c->saved |= g;
}

inline void Save(NrrCtx* c, uint32_t g) {
    if ((c->saved & g) == 0) {
        NrrCoreC3dSaveGroup(c, g);
    }
}

void RestoreUniforms(int loc, const C3D_FVec* v, int n) {
    std::memcpy(&C3D_FVUnif[GPU_VERTEX_SHADER][loc], v, sizeof(C3D_FVec) * (size_t)n);
    for (int i = 0; i < n; i++) {
        C3D_FVUnifDirty[GPU_VERTEX_SHADER][loc + i] = true;
    }
}

/* Every saved group back into citro3d's context, marked dirty: the next draw (the interpreter's)
 * re-emits exactly what the backend had set. */
void NrrCoreC3dRestore(NrrCtx* c) {
    C3D_Context* ctx = C3Di_GetContext();
    const uint32_t g = c->saved;
    u32 dirty = 0;
    if (g & G_ATTR) {
        ctx->attrInfo = sSaved.attr;
        dirty |= C3DiF_AttrInfo;
    }
    if (g & G_BUF) {
        ctx->bufInfo = sSaved.buf;
        dirty |= C3DiF_BufInfo;
    }
    if (g & G_EFFECT) {
        ctx->effect = sSaved.effect;
        dirty |= C3DiF_Effect;
    }
    if (g & G_VP) {
        std::memcpy(ctx->viewport, sSaved.viewport, sizeof(sSaved.viewport));
        std::memcpy(ctx->scissor, sSaved.scissor, sizeof(sSaved.scissor));
        dirty |= C3DiF_Viewport | C3DiF_Scissor;
    }
    if (g & G_TEXENV) {
        std::memcpy(ctx->texEnv, sSaved.texEnv, sizeof(sSaved.texEnv));
        dirty |= C3DiF_TexEnvAll;
    }
    if (g & G_ENVBUF) {
        ctx->texEnvBuf = sSaved.texEnvBuf;
        dirty |= C3DiF_TexEnvBuf;
    }
    if (g & G_TEX) {
        std::memcpy(ctx->tex, sSaved.tex, sizeof(sSaved.tex));
        dirty |= C3DiF_TexAll;
    }
    ctx->flags |= dirty;
    if (g & G_PROJ) {
        RestoreUniforms(gBk.projLoc, sSaved.proj, 4);
    }
    if (g & G_XF) {
        RestoreUniforms(gBk.xfUnifLoc, sSaved.xf, kXfVec4s);
    }
    if (g & G_BOOL) {
        C3D_BoolUnifs[GPU_VERTEX_SHADER] = sSaved.bools;
        C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] = true;
    }
    if (g & G_STK) {
        RestoreUniforms(gBk.stkLoc, &sSaved.stk, 1);
    }
    c->saved = 0;
}

/* -------------------------------------------------------------- state: hot apply ---- */

/* The block's TexEnv stages into citro3d's context; only the stages whose words differ are marked
 * dirty (re-emitted), like C3D_GetTexEnv on the stages a setter touched. */
void NrrCoreC3dTexEnv(NrrCtx* c, const NrrState* s) {
    C3D_Context* ctx = C3Di_GetContext();
    u32 dirty = 0;
    Save(c, G_TEXENV);
    for (int i = 0; i < kStages; i++) {
        if (std::memcmp(&ctx->texEnv[i], &s->env[i], sizeof(C3D_TexEnv)) != 0) {
            ctx->texEnv[i] = s->env[i];
            dirty |= C3DiF_TexEnv(i);
        }
        c->stageConst[i] = s->env[i].color;
    }
    ctx->flags |= dirty;
}

void NrrCoreC3dEffect(NrrCtx* c, const NrrState* s) {
    C3D_Context* ctx = C3Di_GetContext();
    Save(c, G_EFFECT);
    Save(c, G_VP);
    if (std::memcmp(&ctx->effect, &s->effect, sizeof(C3D_Effect)) != 0) {
        ctx->effect = s->effect;
        ctx->flags |= C3DiF_Effect;
    }
    if (!c->keepVp && std::memcmp(ctx->scissor, s->scissor, sizeof(s->scissor)) != 0) {
        std::memcpy(ctx->scissor, s->scissor, sizeof(s->scissor));
        ctx->flags |= C3DiF_Scissor;
    }
}

/* Bind a COPY of the backend's texture with NRR's filter/wrap bits: the backend sets a standalone
 * texture's filter only in SetSamplerParameters, so NRR must never write its params in place. */
void NrrCoreC3dTexUnit(NrrCtx* c, int u, const NrrTexUnit& t) {
    const C3D_Tex* src = static_cast<const C3D_Tex*>(t.obj);
    if (src == nullptr) {
        NrrBkTex bt;
        if (t.id == NRR_TEX_NONE || !gdx3ds_nrr_bk_tex(t.id, &bt)) {
            return;
        }
        src = static_cast<const C3D_Tex*>(bt.tex);
    }
    C3D_Tex& copy = sTexCopy[u];
    const uint32_t param = (src->param & ~t.mask) | t.bits;
    if (sTexSrc[u] == src && copy.data == src->data && copy.param == param) {
        return; /* the unit already holds exactly this */
    }
    Save(c, G_TEX);
    copy = *src;
    copy.param = param;
    sTexSrc[u] = src;
    C3D_TexBind(u, &copy);
}

/* The first draw-state write of a hand-off: the full main target, fog unit off. */
void NrrCoreC3dFirstTouch(NrrCtx* c) {
    Save(c, G_VP);
    Save(c, G_ENVBUF);
    if (!c->keepVp) {
        C3D_SetViewport(0u, 0u, 240u, 400u); /* the main target, full window (portrait) */
    }
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
}

void NrrCoreC3dAttr(NrrCtx* c, int prog) {
    if (c->lastAttr == prog) {
        return;
    }
    Save(c, G_ATTR);
    Save(c, G_BOOL);
    const void* attr = prog == NRR_PROG_2D ? gBk.attrLegacy : (prog == NRR_PROG_XF ? gBk.attrXf : gBk.attrXfF);
    C3D_SetAttrInfo(const_cast<C3D_AttrInfo*>(static_cast<const C3D_AttrInfo*>(attr)));
    if (gBk.xfBoolLoc >= 0) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, gBk.xfBoolLoc, prog != NRR_PROG_2D);
    }
    c->lastAttr = static_cast<int8_t>(prog);
    c->lastVerts = nullptr;
}

/* The 2D projection (gdx3ds_nrr_bk_proj2d: the fixup, or the border mode's UI map) into the
 * projection uniform, as C3D_FVUnifMtx4x4 stores it (row i -> register loc + i, marked dirty). */
__attribute__((noinline)) void NrrCoreC3dProj(NrrCtx* c) {
    Save(c, G_PROJ);
    const C3D_Mtx* m = static_cast<const C3D_Mtx*>(gdx3ds_nrr_bk_proj2d());
    for (int i = 0; i < 4; i++) {
        C3D_FVUnif[GPU_VERTEX_SHADER][gBk.projLoc + i] = m->r[i];
        C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.projLoc + i] = true;
    }
    c->projSet = 1;
}

/* [n60mach] The 3D scene map into the projection uniform (the backend's mono gpuxf draw uploads the
 * same matrix: the fixup, or the border mode's scene matrix); under keepVp the scissor is cut to
 * the game's clip square (NrrClipScissorWords). */
__attribute__((noinline)) void NrrCoreC3dProj3d(NrrCtx* c) {
    Save(c, G_PROJ);
    const C3D_Mtx* m = static_cast<const C3D_Mtx*>(gdx3ds_nrr_bk_proj3d());
    for (int i = 0; i < 4; i++) {
        C3D_FVUnif[GPU_VERTEX_SHADER][gBk.projLoc + i] = m->r[i];
        C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.projLoc + i] = true;
    }
    c->projSet = 2;
    C3D_Context* ctx = C3Di_GetContext();
    u32 cut[3];
    if (c->keepVp && NrrClipScissorWords(m, ctx->scissor, cut)) {
        Save(c, G_VP);
        std::memcpy(ctx->scissor, cut, sizeof(cut)); /* C3D_SetScissor(GPU_SCISSOR_NORMAL, ...) */
        ctx->flags |= C3DiF_Scissor;
    }
}

/* n60stereo: the draw's stereo row into stk (frame replay only; citro3d's array is the shadow). */
__attribute__((noinline)) void NrrCoreC3dStereo(NrrCtx* c, const float* verts2d) {
    const float* row = NrrStereoRow(c, verts2d);
    if (row == nullptr) {
        return;
    }
    const C3D_FVec& cur = C3D_FVUnif[GPU_VERTEX_SHADER][gBk.stkLoc];
    if (cur.x == row[0] && cur.y == row[1] && cur.z == row[2] && cur.w == row[3]) {
        return;
    }
    Save(c, G_STK);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, gBk.stkLoc, row[0], row[1], row[2], row[3]);
}

void NrrCoreC3dBuf(NrrCtx* c, const void* verts, uint32_t stride, int attribs, uint64_t perm) {
    if (c->lastVerts == verts && c->lastStride == stride) {
        return;
    }
    Save(c, G_BUF);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, verts, static_cast<ptrdiff_t>(stride), attribs, perm);
    c->lastVerts = verts;
    c->lastStride = stride;
}

/* gfx_citro3d.cpp PackTexEnvColor, bit for bit (no clamp: the inputs are 0..1 by contract).
 * One out-of-line copy: NrrSetInputs calls it per constant stage. */
__attribute__((noinline)) uint32_t NrrCoreHotPack(float r, float g, float b, float a) {
    const uint32_t ri = static_cast<uint32_t>(r * 255.0f + 0.5f) & 0xFFu;
    const uint32_t gi = static_cast<uint32_t>(g * 255.0f + 0.5f) & 0xFFu;
    const uint32_t bi = static_cast<uint32_t>(b * 255.0f + 0.5f) & 0xFFu;
    const uint32_t ai = static_cast<uint32_t>(a * 255.0f + 0.5f) & 0xFFu;
    return ri | (gi << 8) | (bi << 16) | (ai << 24);
}

void NrrCoreC3dStageConst(NrrCtx* c, int stage, uint32_t packed) {
    if (c->stageConst[stage] == packed) {
        return;
    }
    Save(c, G_TEXENV);
    C3D_Context* ctx = C3Di_GetContext();
    ctx->texEnv[stage].color = packed;
    ctx->flags |= C3DiF_TexEnv(stage);
    c->stageConst[stage] = packed;
}

/* --------------------------------------------------------------------- receipts ---- */

/* niview: multi-view hand-offs per view in the window (gdx3ds_nrr_note_view, called from the categories'
 * multi-view branches only, so the 1P hot path pays nothing): a `[niview]` line when any was counted. */
uint32_t sViewMk[NRR_MAX_VIEWS];

GDX_COLD void EmitWindow() {
    char line[600];
    int n = snprintf(line, sizeof(line), "[nrr] frame=%lu tasks=%u", sTaskTotal, (unsigned)sTasks);
    const double perTask = sTasks != 0 ? 1.0 / sTasks : 0.0;
    for (int cat = 0; cat < NRR_CAT_COUNT && n > 0 && n < (int)sizeof(line); cat++) {
        if (!gdx3ds_nrr_level(cat)) {
            continue;
        }
        const NrrCatStats& s = sStats[cat];
        const double us = (double)s.ticks * perTask * 1000.0 / (double)CPU_TICKS_PER_MSEC;
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      " | %s mk=%.1f dr=%.1f st=%.1f vx=%.0f us=%.1f fb=%u lag=%u", gdx3ds_nrr_cat_name(cat),
                      s.markers * perTask, s.draws * perTask, s.states * perTask, s.verts * perTask, us,
                      (unsigned)s.fallbacks, (unsigned)s.lagMax);
    }
    if (n > 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " | states=%d miss=%u full=%u ring_out=%u bad=%u ovf=%u",
                      sStateCount, (unsigned)sStateMisses, (unsigned)sStateFull, (unsigned)sRingOut,
                      (unsigned)sBadMarkers, (unsigned)gdx3ds_nrr_take_ovf());
    }
    if ((sViewMk[0] | sViewMk[1] | sViewMk[2] | sViewMk[3]) != 0) {
        gdx3ds_nrr_log("[niview] frame=%lu tasks=%u multi-view hand-offs/f by view=%.1f/%.1f/%.1f/%.1f", sTaskTotal,
                       (unsigned)sTasks,
                       sViewMk[0] * perTask, sViewMk[1] * perTask, sViewMk[2] * perTask, sViewMk[3] * perTask);
    }
    std::memset(sViewMk, 0, sizeof(sViewMk));
    gdx3ds_nrr_log("%s", line);
    std::memset(sStats, 0, sizeof(sStats));
    sTasks = 0;
    sStateMisses = 0;
    sRingOut = 0;
    sBadMarkers = 0;
}

GDX_COLD void NoteFallback(int cat, int view, int rc) {
    sStats[cat].fallbacks++;
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbFallback(cat, view, rc);
    }
    if (sStats[cat].fallbacks <= 4) {
        gdx3ds_nrr_log("[nrr] fallback cat=%s view=%d rc=%d frame=%lu", gdx3ds_nrr_cat_name(cat), view, rc,
                       sTaskTotal);
    }
}

GDX_COLD void NoteBadMarker(uint32_t w0) {
    if (++sBadMarkers <= 4) {
        gdx3ds_nrr_log("[nrr] bad marker w0=0x%08lx (frame magic/cat/view)", (unsigned long)w0);
    }
}

GDX_COLD void EmitDirectWindow() {
    NrrDxStats& d = gDxStats;
    gdx3ds_nrr_log("[nrrdx] frame=%lu mode=%d | handoffs=%u syncs=%u midsyncs=%u draws=%u | vfy=%u bad=%u unv=%u endbad=%u "
                   "words c3d=%u dx=%u | first=%u reg=0x%03x c3d=0x%08lx dx=0x%08lx",
                   sTaskTotal, gDirect, (unsigned)d.handoffs, (unsigned)d.syncs, (unsigned)d.midSyncs, (unsigned)d.draws,
                   (unsigned)d.verified, (unsigned)d.badDraw, (unsigned)d.unknown, (unsigned)d.badEnd,
                   (unsigned)d.wordsC3d, (unsigned)d.wordsDx, (unsigned)d.firstBadKind, (unsigned)d.firstBadReg,
                   (unsigned long)d.firstBadC3d, (unsigned long)d.firstBadDx);
    NrrDxDrawWindow(sTaskTotal); /* n60draw */
    const NrrDxStats keep = d;
    std::memset(&d, 0, sizeof(d));
    d.firstBadKind = keep.firstBadKind; /* the first mismatch is sticky: it names the draw since boot */
    d.firstBadReg = keep.firstBadReg;
    d.firstBadC3d = keep.firstBadC3d;
    d.firstBadDx = keep.firstBadDx;
}

/* One combiner constant on the citro3d path, or under the verify (nrr_direct 0 / 2). */
__attribute__((noinline)) void NrrCoreC3dConstAny(NrrCtx* c, int stage, uint32_t packed) {
    if (gDirect == 2) {
        NrrDxVerifyConst(c, stage, packed);
    } else {
        NrrCoreC3dStageConst(c, stage, packed);
    }
}

/* One combiner constant for the emitter in use. nrrdiet: plain noinline (not `inline`) -- it is
 * called from a loop (NrrSetInputs, up to 6 stages) and from NrrSetFog, and being `inline` had
 * GCC duplicate this whole gDirect dispatch (both the fast-path call and the dead-for-production
 * NrrCoreC3dConstAny arm) at every one of those call sites instead of sharing one copy. "HotConst"
 * matches no nrr_size.sh hot pattern, so forcing a single out-of-line copy is invisible to the
 * budget on top of not being duplicated. */
__attribute__((noinline)) void HotConst(NrrCtx* c, int stage, uint32_t packed) {
    if (GDX_LIKELY(gDirect == 1)) {
        NrrCoreDxConst(stage, packed);
    } else {
        NrrCoreC3dConstAny(c, stage, packed);
    }
}

} // namespace

/* [n60mach] The game's clip square (x, y in [-w, w]) on the main target under the 3D map `m`, as a
 * scissor intersected with the walk's: what the interpreter's CPU clipper cuts at for a mesh that
 * straddles the view, where the PICA (which clips to the full target) would draw on into a
 * neighbouring split-screen view or the border. Viewport = the full main target (240 x 400). */
GDX_COLD int nrrcore::NrrClipScissorWords(const C3D_Mtx* m, const u32 cur[3], u32 out[3]) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        const float cx = (k & 1) ? 1.0f : -1.0f;
        const float cy = (k & 2) ? 1.0f : -1.0f;
        const float ox = m->r[0].x * cx + m->r[0].y * cy + m->r[0].w;
        const float oy = m->r[1].x * cx + m->r[1].y * cy + m->r[1].w;
        const float ow = m->r[3].x * cx + m->r[3].y * cy + m->r[3].w;
        if (ow <= 0.0f) {
            return 0;
        }
        const float px = (ox / ow + 1.0f) * 120.0f;
        const float py = (oy / ow + 1.0f) * 200.0f;
        x0 = px < x0 ? px : x0;
        x1 = px > x1 ? px : x1;
        y0 = py < y0 ? py : y0;
        y1 = py > y1 ? py : y1;
    }
    int l = x0 < 0.0f ? 0 : (int)(x0 + 0.5f), t = y0 < 0.0f ? 0 : (int)(y0 + 0.5f);
    int r = x1 > 240.0f ? 240 : (int)(x1 + 0.5f), b = y1 > 400.0f ? 400 : (int)(y1 + 0.5f);
    if (cur[0] == GPU_SCISSOR_NORMAL) {
        const int sl = (int)(cur[1] & 0xFFFFu), st = (int)(cur[1] >> 16);
        const int sr = (int)(cur[2] & 0xFFFFu) + 1, sb = (int)(cur[2] >> 16) + 1;
        l = l > sl ? l : sl;
        t = t > st ? t : st;
        r = r < sr ? r : sr;
        b = b < sb ? b : sb;
    }
    if (l <= 0 && t <= 0 && r >= 240 && b >= 400) {
        return 0; /* the square covers the target: nothing to cut */
    }
    if (r <= l || b <= t) {
        r = l + 1;
        b = t + 1;
    }
    out[0] = GPU_SCISSOR_NORMAL;
    out[1] = ((u32)t << 16) | ((u32)l & 0xFFFFu);
    out[2] = ((u32)(b - 1) << 16) | ((u32)(r - 1) & 0xFFFFu);
    return 1;
}

/* ------------------------------------------------------------- the citro3d path ---- */

void nrrcore::NrrCoreC3dApply(NrrCtx* c, const NrrState* s) {
    if ((c->saved & G_ENVBUF) == 0) {
        NrrCoreC3dFirstTouch(c);
    }
    NrrCoreC3dTexEnv(c, s);
    NrrCoreC3dEffect(c, s);
    NrrCoreC3dTexUnit(c, 0, s->tex[0]);
    NrrCoreC3dTexUnit(c, 1, s->tex[1]);
}

void nrrcore::NrrCoreC3dConst(NrrCtx* c, int stage, uint32_t packed) {
    NrrCoreC3dStageConst(c, stage, packed);
}

void nrrcore::NrrCoreC3dDraw2D(NrrCtx* c, const float* verts, uint32_t nVerts) {
    if ((c->saved & G_ENVBUF) == 0) {
        NrrCoreC3dFirstTouch(c);
    }
    NrrCoreC3dAttr(c, NRR_PROG_2D);
    if (c->scene2d) {
        if (c->projSet != 2) {
            NrrCoreC3dProj3d(c);
        }
    } else if (c->projSet != 1) {
        NrrCoreC3dProj(c);
    }
    NrrCoreC3dBuf(c, verts, GDX3DS_VBOPACK_STRIDE * sizeof(float), 4, 0x3210);
    if (GDX_UNLIKELY(gdx3ds_stereo_replay_frame)) {
        NrrCoreC3dStereo(c, verts);
    }
    C3D_DrawArrays(GPU_TRIANGLES, 0, static_cast<int>(nVerts));
}

void nrrcore::NrrCoreC3dDrawXf(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
                               const float* unif28x4) {
    if ((c->saved & G_ENVBUF) == 0) {
        NrrCoreC3dFirstTouch(c);
    }
    NrrCoreC3dAttr(c, stride == 16 ? NRR_PROG_XF : NRR_PROG_XF_F);
    if (c->projSet != 2) {
        NrrCoreC3dProj3d(c);
    }
    Save(c, G_XF);
    for (int i = 0; i < kXfVec4s; i++) {
        const float* v = unif28x4 + i * 4;
        const C3D_FVec& cur = C3D_FVUnif[GPU_VERTEX_SHADER][gBk.xfUnifLoc + i];
        if (cur.x == v[0] && cur.y == v[1] && cur.z == v[2] && cur.w == v[3]) {
            continue; /* citro3d's own uniform array is the shadow */
        }
        C3D_FVUnifSet(GPU_VERTEX_SHADER, gBk.xfUnifLoc + i, v[0], v[1], v[2], v[3]);
    }
    NrrCoreC3dBuf(c, verts, stride, 3, 0x210);
    if (GDX_UNLIKELY(gdx3ds_stereo_replay_frame)) {
        NrrCoreC3dStereo(c, nullptr);
    }
    C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(nIdx), C3D_UNSIGNED_SHORT, idx);
}

/* ------------------------------------------------------------------- public: draws ---- */

extern "C" GDX_COLD void gdx3ds_nrr_note_view(int view) {
    sViewMk[view & (NRR_MAX_VIEWS - 1)]++;
}

extern "C" void gdx3ds_nrr_set_draw(int cat, NrrDrawFn fn) {
    if (cat >= 0 && cat < NRR_CAT_COUNT) {
        sDraw[cat] = fn;
    }
}

extern "C" void NrrBegin(NrrCtx* c) {
    if (c->seam != nullptr) {
        static_cast<GdxInterpSeam*>(c->seam)->Flush(); /* painter's order: the open batch first */
    }
    c->last = nullptr;
    c->lastVerts = nullptr;
    c->lastStride = 0;
    c->lastAttr = -1;
    c->saved = 0;
    c->projSet = 0;
    for (int i = 0; i < 6; i++) {
        c->stageConst[i] = 0xFFFFFFFFu; /* unknown: the first write always goes out */
    }
    sTexSrc[0] = sTexSrc[1] = nullptr;
    gdx3ds_nrr_handoff_open = 1;
    if (GDX_UNLIKELY(gdx3ds_skylate_marking != 0)) {
        gdx3ds_skylate_note_nrr(c->cat); /* n60tail: a sky draw after the TRK marker keeps the order */
    }
    if (gDirect != 0) {
        NrrCoreDxBegin();
    }
}

/* nrrdiet: nrr_direct is latched once at boot ([native] nrr_direct); for the production direct
 * emitter (=1, the race/sky/trk/mach/course/menu/machmenu/course build) this tail (the verify
 * end and the citro3d path's own group restore) never runs. Out of line and cold so NrrEnd's own
 * steady body stays the one real call. */
GDX_COLD void NrrEndOther(NrrCtx* c) {
    if (gDirect == 2) {
        NrrDxVerifyEnd();
    }
    if (c->saved != 0) {
        NrrCoreC3dRestore(c);
    }
}

extern "C" void NrrEnd(NrrCtx* c) {
    gdx3ds_nrr_handoff_open = 0;
    if (GDX_LIKELY(gDirect == 1)) {
        NrrCoreDxEnd(c);
        return;
    }
    NrrEndOther(c);
}

namespace {

/* One category draw between NrrBegin and NrrEnd; nonzero = the category's fallback code. */
int NrrCoreHotRun(void* seam, int cat, int view, const NrrFrame* f) {
    NrrCtx c;
    std::memset(&c, 0, sizeof(c));
    c.cat = cat;
    c.view = view;
    c.seam = seam;
    c.st = &sStats[cat];
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbHandoffBegin(cat, view, f);
    }
    NrrBegin(&c);
    const int rc = sDraw[cat](&c, view, f);
    NrrEnd(&c);
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbHandoffEnd(cat, rc, c.saved);
    }
    return rc;
}

/* The same under the [c5cat] clock (cold: the clock is a profiling mode). */
GDX_COLD int RunClocked(void* seam, int cat, int view, const NrrFrame* f) {
    const long long t0 = gdx3ds_prof_now();
    const int rc = NrrCoreHotRun(seam, cat, view, f);
    sStats[cat].ticks += static_cast<uint64_t>(gdx3ds_prof_now() - t0);
    return rc;
}

} // namespace

namespace {

/* n60trkv: a category holding peeked markers (gdx3ds_nrr_set_pending), -1 none. */
int sPendCat = -1;

/* The pending category's held markers, drawn in a hand-off of its own before another category's. */
GDX_COLD void DrainPending(void* seam, const NrrFrame* f) {
    const int pend = sPendCat;
    sPendCat = -1;
    if (sDraw[pend] == nullptr) {
        return;
    }
    const int rc = ClockOn() ? RunClocked(seam, pend, NRR_VIEW_DRAIN, f) : NrrCoreHotRun(seam, pend, NRR_VIEW_DRAIN, f);
    if (rc != 0) {
        NoteFallback(pend, NRR_VIEW_DRAIN, rc);
    }
}

/* The peek under the [c5cat] clock: its time is the category's, like a hand-off's. */
GDX_COLD int PeekClocked(void* seam, int cat, int view, const NrrFrame* f) {
    const long long t0 = gdx3ds_prof_now();
    const int took = sPeek[cat](seam, view, f);
    sStats[cat].ticks += static_cast<uint64_t>(gdx3ds_prof_now() - t0);
    return took;
}

} // namespace

extern "C" void gdx3ds_nrr_set_peek(int cat, NrrPeekFn fn) {
    if (cat >= 0 && cat < NRR_CAT_COUNT) {
        sPeek[cat] = fn;
    }
}

extern "C" void gdx3ds_nrr_set_pending(int cat, int pending) {
    if (pending) {
        sPendCat = cat;
    } else if (sPendCat == cat) {
        sPendCat = -1;
    }
}

extern "C" void gdx3ds_nrr_draw(void* seam, int cat, int view, const NrrFrame* f) {
    if (cat < 0 || cat >= NRR_CAT_COUNT || !gdx3ds_nrr_level(cat)) {
        return;
    }
    NrrCatStats& st = sStats[cat];
    st.markers++;
    const uint32_t lag = gdx3ds_nrr_live_tick() - NrrFrameTick(f);
    if (lag > st.lagMax) {
        st.lagMax = lag;
    }
    if (sDraw[cat] == nullptr || !gdx3ds_nrr_bk_frame_active() || !BackendLatch()) {
        NoteFallback(cat, view, -1);
        return;
    }
    if (GDX_UNLIKELY(sPendCat >= 0) && sPendCat != cat) {
        DrainPending(seam, f);
    }
    if (sPeek[cat] != nullptr && (ClockOn() ? PeekClocked(seam, cat, view, f) : sPeek[cat](seam, view, f)) != 0) {
        return;
    }
    const int rc = ClockOn() ? RunClocked(seam, cat, view, f) : NrrCoreHotRun(seam, cat, view, f);
    if (rc != 0) {
        NoteFallback(cat, view, rc);
    }
}


extern "C" int gdx3ds_nrr_open(NrrCtx* c, void* seam, int cat) {
    if (cat < 0 || cat >= NRR_CAT_COUNT || !gdx3ds_nrr_level(cat) || !gdx3ds_nrr_bk_frame_active() ||
        !BackendLatch()) {
        return 0;
    }
    if (GDX_UNLIKELY(sPendCat >= 0) && sPendCat != cat) {
        DrainPending(seam, nullptr); /* n60trkv: held markers draw before a category's own hand-off */
    }
    std::memset(c, 0, sizeof(*c));
    c->cat = cat;
    c->seam = seam;
    c->st = &sStats[cat];
    sStats[cat].markers++;
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbHandoffBegin(cat, -1, nullptr);
    }
    NrrBegin(c);
    return 1;
}

extern "C" int gdx3ds_nrr_open_ext(NrrCtx* c, void* seam, NrrCatStats* st, int crumbCat) {
    if (crumbCat < 0 || crumbCat >= NRR_CAT_COUNT || !gdx3ds_nrr_on() || !gdx3ds_nrr_bk_frame_active() ||
        !BackendLatch()) {
        return 0;
    }
    if (GDX_UNLIKELY(sPendCat >= 0)) {
        DrainPending(seam, nullptr); /* n60trkv: held markers draw first (painter's order) */
    }
    std::memset(c, 0, sizeof(*c));
    c->cat = crumbCat;
    c->seam = seam;
    c->st = st;
    st->markers++;
    NrrBegin(c);
    return 1;
}

extern "C" void gdx3ds_nrr_close(NrrCtx* c) {
    NrrEnd(c);
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbHandoffEnd(c->cat, 0, c->saved);
    }
}

extern "C" void gdx3ds_nrr_marker(void* seam, uint32_t w0, uint32_t w1) {
    const int cat = static_cast<int>(w0 & 0xFFu);
    const int view = static_cast<int>((w0 >> 8) & 0xFFu);
    const NrrFrame* f = reinterpret_cast<const NrrFrame*>(static_cast<uintptr_t>(w1));
    if (f == nullptr || f->magic != NRR_FRAME_MAGIC || cat >= NRR_CAT_COUNT || view >= NRR_MAX_VIEWS) {
        NoteBadMarker(w0);
        return;
    }
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbMarker(cat, view, f);
    }
    gdx3ds_nrr_draw(seam, cat, view, f);
}

/* [debug] nicensus (n64/gdx_nicensus.cpp): the current window's per-category markers / draws / verts,
 * read only (the caller takes deltas across one task, before gdx3ds_nrr_task_end may reset them). */
extern "C" void gdx3ds_nrr_census_read(uint32_t* markers, uint32_t* draws, uint32_t* verts, int n) {
    for (int cat = 0; cat < n; cat++) {
        const bool in = cat < NRR_CAT_COUNT;
        markers[cat] = in ? sStats[cat].markers : 0;
        draws[cat] = in ? sStats[cat].draws : 0;
        verts[cat] = in ? sStats[cat].verts : 0;
    }
}

extern "C" void gdx3ds_nrr_task_end(void) {
    if (!gdx3ds_nrr_on()) {
        return;
    }
    sTaskTotal++;
    sPendCat = -1; /* n60trkv: markers held past the task are the category's to drop (it counts them) */
    if (gDirect != 0) {
        NrrCoreDxTaskEnd(); /* n60draw: no hand-off carries GPU state across a render task */
    }
    if (++sTasks >= kWindow) {
        if (gDirect != 0) {
            EmitDirectWindow();
        }
        EmitWindow();
    }
}

/* ------------------------------------------------------------- public: state ---- */

extern "C" const NrrState* NrrStateGet(const NrrStateKey* k) {
    for (int i = 0; i < sStateCount; i++) {
        if (std::memcmp(&sStates[i].key, k, sizeof(NrrStateKey)) == 0) {
            return &sStates[i];
        }
    }
    return StateBuild(k);
}

extern "C" uint32_t NrrStateBytes(void) {
    return (uint32_t)((sizeof(NrrState) + 7u) & ~7u);
}

extern "C" const NrrState* NrrStateBuildInto(const NrrStateKey* k, void* mem) {
    NrrState* s = static_cast<NrrState*>(mem);
    const bool ok = s != nullptr && StateFill(*s, k);
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbState(k, ok ? s : nullptr, true);
    }
    return ok ? s : nullptr;
}

extern "C" const NrrState* NrrStateRescissor(const NrrState* base, void* mem, int x, int y, int w, int h) {
    if (base == nullptr || mem == nullptr) {
        return nullptr;
    }
    NrrState* s = static_cast<NrrState*>(mem);
    std::memcpy(static_cast<void*>(s), base, sizeof(NrrState));
    C3D_Context* ctx = C3Di_GetContext();
    u32 savedScissor[3];
    std::memcpy(savedScissor, ctx->scissor, sizeof(savedScissor));
    const u32 savedFlags = ctx->flags;
    if (x == 0 && y == 0 && w == 0 && h == 0) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0); /* == BuildEffect: all 0 = off */
    } else {
        gdx3ds_nrr_bk_scissor(x, y, w, h);
    }
    std::memcpy(s->scissor, ctx->scissor, sizeof(s->scissor));
    std::memcpy(ctx->scissor, savedScissor, sizeof(savedScissor));
    ctx->flags = savedFlags;
    s->key.scissor[0] = static_cast<int16_t>(x); /* descriptive only: this block is in no table */
    s->key.scissor[1] = static_cast<int16_t>(y);
    s->key.scissor[2] = static_cast<int16_t>(w);
    s->key.scissor[3] = static_cast<int16_t>(h);
    NrrDxRescissor(*s);
    return s;
}

extern "C" uint32_t NrrStateGeneration(void) {
    return sStateGen;
}

extern "C" void gdx3ds_nrr_state_flush(void) {
    sStateCount = 0;
    sStateGen++;
}

/* nrrdiet: same split as NrrEndOther -- nrr_direct's verify (2) and citro3d (0) emitters, dead
 * for the production direct build (=1). */
GDX_COLD void NrrStateApplyOther(NrrCtx* c, const NrrState* s) {
    if (gDirect == 2) {
        NrrDxVerifyApply(c, s);
    } else {
        NrrCoreC3dApply(c, s);
    }
}

extern "C" void NrrStateApply(NrrCtx* c, const NrrState* s) {
    if (s == nullptr || s == c->last) {
        return;
    }
    if (GDX_LIKELY(gDirect == 1)) {
        NrrCoreDxApply(c, s);
    } else {
        NrrStateApplyOther(c, s);
    }
    c->last = s;
    c->st->states++;
}

extern "C" void NrrSetInputs(NrrCtx* c, const NrrState* s, const float (*in)[4], int n) {
    static const float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    static const float kOne[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    for (int k = 0; k < s->numConst; k++) {
        const NrrConstSrc& cs = s->cs[k];
        const float* rgb = (cs.rgbIn != 0 && cs.rgbIn <= n) ? in[cs.rgbIn - 1] : (cs.rgbOne ? kOne : kZero);
        const float* a = (cs.aIn != 0 && cs.aIn <= n) ? in[cs.aIn - 1] : (cs.aOne ? kOne : kZero);
        HotConst(c, cs.stage, NrrCoreHotPack(rgb[0], rgb[1], rgb[2], a[3]));
    }
}

extern "C" void NrrSetFog(NrrCtx* c, const NrrState* s, const float rgb[3]) {
    if (s->sh.fogStage >= 0) {
        HotConst(c, s->sh.fogStage, NrrCoreHotPack(rgb[0], rgb[1], rgb[2], 1.0f));
    }
}

extern "C" int NrrStateFogStage(const NrrState* s) {
    return s->sh.fogStage;
}

extern "C" int NrrStateVtxColorInput(const NrrState* s) {
    return s->sh.vtxColorInput;
}

extern "C" int NrrStateVtxAlphaInput(const NrrState* s) {
    return s->sh.vtxAlphaInput;
}

extern "C" int NrrStateTexXform(const NrrState* s, int unit, float scale[2], float off[2]) {
    NrrBkTex bt;
    if (unit < 0 || unit > 1 || s->tex[unit].id == NRR_TEX_NONE || !gdx3ds_nrr_bk_tex(s->tex[unit].id, &bt)) {
        return 0;
    }
    scale[0] = bt.uScale;
    scale[1] = bt.vScale;
    off[0] = bt.uOff;
    off[1] = bt.vOff;
    return 1;
}

/* ------------------------------------------------------------ public: ring + draw ---- */

extern "C" void* NrrRingAlloc(NrrCtx* c, uint32_t bytes) {
    void* p = gdx3ds_nrr_bk_alloc(bytes);
    if (p == nullptr) {
        sRingOut++;
    }
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbRing(c, bytes, p);
    }
    return p;
}

extern "C" void NrrDraw2D(NrrCtx* c, const float* verts, uint32_t nVerts) {
    if (nVerts == 0) {
        return;
    }
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbDraw(c, false, verts, GDX3DS_VBOPACK_STRIDE * sizeof(float), nullptr, nVerts);
    }
    if (gDirect == 1) {
        NrrCoreDxDraw2D(c, verts, nVerts);
    } else if (gDirect == 2) {
        NrrDxVerifyDraw2D(c, verts, nVerts);
    } else {
        NrrCoreC3dDraw2D(c, verts, nVerts);
    }
    c->anchorSet = 0;
    c->st->draws++;
    c->st->verts += nVerts;
}

extern "C" void NrrStereoAnchor(NrrCtx* c, float primDepth) {
    if (!gdx3ds_stereo_replay_frame) {
        return;
    }
    gdx3ds_nrr_bk_anchor_row(primDepth, c->anchorRow);
    c->anchorSet = 1;
}

extern "C" GDX_COLD void NrrStereoMenuRow(NrrCtx* c, float** verts, uint32_t* nVerts) {
    if (!gdx3ds_stereo_replay_frame) {
        return;
    }
    float* const v = *verts;
    const uint32_t n = *nVerts;
    uint32_t first = 0;
    uint32_t end = gdx3ds_stereo_menu_run_end(v, n, GDX3DS_VBOPACK_STRIDE, 0u);
    while (end < n) {
        float* const run = v + (size_t)first * GDX3DS_VBOPACK_STRIDE;
        gdx3ds_stereo_menu_ui_row(run, end - first, GDX3DS_VBOPACK_STRIDE, c->anchorRow);
        c->anchorSet = 1;
        NrrDraw2D(c, run, end - first);
        first = end;
        end = gdx3ds_stereo_menu_run_end(v, n, GDX3DS_VBOPACK_STRIDE, first);
    }
    *verts = v + (size_t)first * GDX3DS_VBOPACK_STRIDE;
    *nVerts = n - first;
    gdx3ds_stereo_menu_ui_row(*verts, *nVerts, GDX3DS_VBOPACK_STRIDE, c->anchorRow);
    c->anchorSet = 1;
}

extern "C" void NrrDraw2DScene(NrrCtx* c, const float* verts, uint32_t nVerts) {
    c->scene2d = 1;
    NrrDraw2D(c, verts, nVerts);
    c->scene2d = 0;
}

extern "C" void NrrDrawXfRows(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
                              const float* unif28x4, uint32_t rows) {
    if (nIdx == 0 || gBk.xfUnifLoc < 0) {
        return;
    }
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbDraw(c, true, verts, stride, idx, nIdx);
    }
    if (gDirect == 1) {
        NrrCoreDxDrawXf(c, verts, stride, idx, nIdx, unif28x4, rows);
    } else if (gDirect == 2) {
        NrrDxVerifyDrawXf(c, verts, stride, idx, nIdx, unif28x4, rows);
    } else {
        NrrCoreC3dDrawXf(c, verts, stride, idx, nIdx, unif28x4);
    }
    c->st->draws++;
    c->st->verts += nIdx;
}

extern "C" void NrrProjInvalidate(NrrCtx* c) {
    c->projSet = 0;
    if (gDirect != 0) {
        NrrCoreDxProjInvalidate();
    }
}

extern "C" void NrrDrawXf(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
                          const float* unif28x4) {
    NrrDrawXfRows(c, verts, stride, idx, nIdx, unif28x4, kXfAllRows);
}
