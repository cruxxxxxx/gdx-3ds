/* port/3ds/gfx/nrr/gdx3ds_nrr_direct.cpp -- [native] nrr_direct=1: the NRR's GPU command words,
 * written straight into citro3d's command buffer (n60core2, docs/research/native/n60core2-progress.md).
 *
 * The citro3d path (nrr_direct=0) stores NRR state into citro3d's context and lets C3D_DrawArrays /
 * C3D_DrawElements run C3Di_UpdateContext, which walks every dirty group (effect, textures, TexEnv,
 * buffers, attributes, both shaders' uniform files) per draw: ~2,000 emulator instructions and
 * ~5 KB of citro3d code per NRR draw. Here every word is precomputed or patched:
 *   - per state block (cold, NrrStateGet): the scissor, effect and six TexEnv stages as finished
 *     packets (NrrState::dx), applied with one copy;
 *   - per attribute layout (cold, latch): the attribute words and a 40-word buffer template, and
 *     a buffer change inside a layout is ONE register write (ATTRIBBUFFER0_OFFSET);
 *   - per draw: the uniform words that changed, then the draw packets (a 22 / 30-word template
 *     with the count and index offset patched);
 *   - a TexEnv constant (NrrSetInputs / NrrSetFog) is one TEXENVi_COLOR write.
 * The packet layout is libctru GPUCMD_AddInternal's (first parameter, header with count-1 in bits
 * 20-27, the other parameters, a zero pad word when count-1 is odd).
 *
 * citro3d's context is never written: the GPU holds what citro3d says at NrrBegin (pending dirty
 * state is emitted first, C3Dgdx_FlushState), the direct words change the GPU behind citro3d's
 * back, and NrrEnd marks every group they touched dirty so citro3d's next draw (the interpreter's)
 * re-emits its own state. Exactness against the citro3d path is checked per draw by
 * nrr_direct=2 (gdx3ds_nrr_dxverify.cpp).
 *
 * HOT: NrrCoreDx* (tools/native/nrr_size.sh counts them as the direct core). Cold: the builders. */
#include "gdx3ds_nrr_core.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "../gdx3ds_hotcold.h"
#include "../gdx3ds_vbopack.h"
#include "../../vendor/citro3d/gdx/c3d_gdx.h"
#include "gdx3ds_nrr_crumb.h" /* n60hwdiag */

using namespace nrrcore;

nrrcore::NrrDxStats nrrcore::gDxStats = {};
uint32_t nrrcore::gDraw = 0;

extern "C" int gdx3ds_native_raw(const char* key, int fallback) __attribute__((weak));

namespace {

constexpr uint32_t kBufferBasePaddr = 0x18000000u; /* citro3d buffers.c BUFFER_BASE_PADDR */
constexpr uint32_t kBufWords = 40u;                /* LOC (2) + the 12 buffer configs (38) */
constexpr uint32_t kAttrWords = 12u;
constexpr uint32_t kArraysWords = 22u;
constexpr uint32_t kElementsWords = 30u;
constexpr int kLayouts = 3; /* NRR_PROG_2D, NRR_PROG_XF, NRR_PROG_XF_F */

constexpr uint32_t Hdr(uint32_t reg, uint32_t mask) {
    return (mask << 16) | reg;
}

/* A packet of `count` parameters: incremental (consecutive registers) or not (one data port). */
constexpr uint32_t HdrInc(uint32_t reg, uint32_t count) {
    return 0x80000000u | (0xFu << 16) | ((count - 1u) << 20) | reg;
}

constexpr uint32_t HdrPort(uint32_t reg, uint32_t count) {
    return (0xFu << 16) | ((count - 1u) << 20) | reg;
}

constexpr uint32_t TexEnvReg(int stage) {
    return GPUREG_TEXENV0_SOURCE + 8u * static_cast<uint32_t>(stage < 4 ? stage : stage + 2);
}

/* One attribute layout: its attribute words (+ the xfon bool packet, value patched at word 12) and
 * its buffer template (offset patched at word 2). */
struct DxLayout {
    uint32_t attr[kAttrWords + 2u];
    uint32_t attrCount; /* 12, or 14 with the bool packet */
    uint32_t buf[kBufWords];
    uint32_t stride; /* the template's stride; 0 = not built */
    int attribs;
    uint64_t perm;
};

/* What this hand-off's direct words left on the GPU. */
struct DxHandoff {
    uint32_t touched;      /* G_* groups the direct words changed (NrrCoreDxEnd dirties them) */
    int8_t attr;           /* NRR_PROG_* layout emitted, -1 none */
    int8_t bufLayout;      /* layout whose full buffer config was emitted, -1 none */
    uint8_t proj;          /* 1: the 2D fixup, 2: the 3D scene map, 0: citro3d's */
    uint8_t texBound;      /* texture units bound */
    uint8_t xfSeeded;      /* xf holds the GPU's gpuxf block */
    uint8_t stkSeeded;     /* n60stereo: stk holds the GPU's stereo row */
    uint8_t firstDone;     /* n60draw: this hand-off's first-touch words went out */
    uint8_t xfHint;        /* n60draw: xf equals this hand-off's previous gpuxf block (NrrDrawXfRows) */
    uint32_t deferred;     /* n60draw carry: citro3d flags pending at a carried begin (NrrCoreDxEnd re-sets) */
    uint32_t lazy;         /* n60draw carry: ...of which still to emit before the first draw */
    uint32_t lazyTexConfig; /* n60draw carry: citro3d's texConfig at the carried begin */
    uint32_t bufStride;
    uint32_t bufOff;       /* buffer 0 offset emitted */
    uint32_t stageConst[kStages];
    uint16_t group[kDxGroups]; /* content id per state group the GPU holds (0xFFFF unknown) */
    const void* texSrc[2];
    const void* texData[2];
    uint32_t texParam[2];
    C3D_FVec xf[kXfVec4s]; /* the gpuxf block the GPU holds (valid with xfSeeded) */
    C3D_FVec stk;          /* n60stereo: the stereo row the GPU holds (valid with stkSeeded) */
};

DxHandoff sDx;
DxLayout sLayout[kLayouts];

/* n60draw carry: where the previous hand-off's words ended. A begin finding the command stream
 * exactly there (same list, same offset, no queue entry added, same render task) knows nobody
 * wrote a GPU register in between, so the GPU still holds what sDx says. */
struct DxEndMark {
    const u32* buf;
    u32 off;
    u16 entries;
    bool valid;
    uint32_t epoch;
    u32 endFlags; /* the flags that end marked (under the verify the citro3d path's restore marks others) */
};
DxEndMark sMark = {};
uint32_t sEpoch = 0; /* render tasks (NrrCoreDxTaskEnd) */

/* Groups a carried begin leaves pending: what sDx tracks (attributes, buffers, effect, stages, the
 * TexEnv buffer: the first touch re-sends it) and what the hand-off re-emits from citro3d's context
 * itself before its first draw (viewport, scissor, texture units and status). Anything else pending
 * (a program, a framebuffer, procedural textures, lights, a fog LUT with fog on) refuses the carry. */
constexpr u32 kCarryFlags = C3DiF_AttrInfo | C3DiF_BufInfo | C3DiF_Effect | C3DiF_TexEnvAll | C3DiF_TexEnvBuf |
                            C3DiF_Viewport | C3DiF_Scissor | C3DiF_TexAll | C3DiF_TexStatus;
/* Of those, what citro3d's begin flush would have put on the GPU and NRR's own words may not
 * overwrite before the first draw: the first touch settles viewport + scissor, an apply settles
 * effect + stages, the first draw settles the texture units nobody bound. */
constexpr u32 kLazyFlags = C3DiF_Viewport | C3DiF_Scissor | C3DiF_TexAll | C3DiF_TexStatus | C3DiF_Effect |
                           C3DiF_TexEnvAll;

/* n60draw receipt counters (one window, [nrrdraw]). */
struct DrawStats {
    uint32_t adjacent;     /* begins with no GPU word since the previous end */
    uint32_t carried;
    uint32_t refFlags;     /* adjacent, refused: a context group outside kCarryFlags pending */
    uint32_t refUnif;      /* adjacent, refused: a uniform written since (citro3d's touch hint) */
    uint32_t lazyVp;       /* carried: citro3d's viewport / scissor emitted at the first touch (keepVp) */
    uint32_t lazyTex;      /* carried: citro3d's texture units / status emitted before the first draw */
    uint32_t lazyState;    /* carried: citro3d's effect / stages emitted (a draw before any apply) */
    uint32_t guardFast;    /* draws whose sync test the hint settled */
    uint32_t guardFull;    /* ...that scanned (StatePending) */
    uint32_t xfDraws;
    uint32_t xfHinted;     /* gpuxf blocks compared by the caller's row mask */
    uint32_t xfRows;       /* rows compared */
};
DrawStats sDrawSt = {};

/* Templates: values marked "patched" are written per use. */
uint32_t sViewportWords[12];  /* viewport (8) + scissor off (4: words 10, 11 patched) */
uint32_t sEnvBufWords[6];     /* TexEnv buffer, buffer colour, fog colour (words 0, 2, 4 patched) */
uint32_t sProjWords[20];      /* uniform CONFIG + a 16-word DATA packet (words 0, 2, 4..18 patched) */
uint32_t sTexWords[2][8];     /* per unit: border..addr (5) + type (words 0, 2..6 patched) */
uint32_t sTexStatusWords[6];  /* unit enables, cache clear, shadow (words 0, 4 patched) */
uint32_t sArraysWords[kArraysWords];
uint32_t sElementsWords[kElementsWords];

/* The groups of NrrState::dx: word offset and length. */
constexpr uint8_t kGroupOff[kDxGroups + 1] = { 0, 4, 38, 44, 50, 56, 62, 68, 74 };
constexpr uint32_t GroupLen(int g) {
    return kGroupOff[g + 1] - kGroupOff[g];
}

/* Interned group contents (cold): one list per group; the id is the index. */
std::vector<std::vector<uint32_t>> sGroupSeen[kDxGroups];

/* ---------------------------------------------------------------- the writer ---- */

/* n3dsfix (docs/research/native/hw-n60-3d7-analysis.md): same durable pre-panic breadcrumb as
 * gdx_submit.h's C3Dgdx_Reserve/PutChunk -- this is the OTHER bounds-checked GPU command buffer
 * writer (the nrr_direct=1 fast path), so an overflow here is diagnosable from the card alone
 * instead of needing an addr2line pass. Weak + null-checked: zero-cost before gdx3ds_filelog is up. */
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern "C" void gdx3ds_filelog_flush(void) __attribute__((weak));
extern "C" void gdx3ds_filelog_flush_sync(void) __attribute__((weak)); /* o3ds-cpucuts: svcBreak next */
GDX_COLD void DxOverflowLog(uint32_t n, uint32_t off, uint32_t size) {
    if (gdx3ds_filelog_write == nullptr) {
        return;
    }
    char buf[128];
    int rc = std::snprintf(buf, sizeof(buf), "[cmdbuf] OVERFLOW NrrCoreDxRes words=%lu off=%lu size=%lu (panicking)\n",
                            (unsigned long)n, (unsigned long)off, (unsigned long)size);
    if (rc > 0) {
        gdx3ds_filelog_write(buf, (size_t)rc);
        if (gdx3ds_filelog_flush_sync != nullptr) {
            gdx3ds_filelog_flush_sync();
        } else if (gdx3ds_filelog_flush != nullptr) {
            gdx3ds_filelog_flush();
        }
    }
}

/* GPUCMD_AddInternal's reaction to a full command buffer. */
GDX_COLD __attribute__((noreturn)) void DxOverflow(uint32_t n, uint32_t off, uint32_t size) {
    DxOverflowLog(n, off, size);
    svcBreak(USERBREAK_PANIC);
    __builtin_unreachable();
}

/* Room for n words in the command buffer (GPUCMD_AddInternal's bounds test). */
__attribute__((noinline)) uint32_t* NrrCoreDxRes(uint32_t n) {
    const uint32_t off = gpuCmdBufOffset;
    if (off + n > gpuCmdBufSize) {
        DxOverflow(n, off, gpuCmdBufSize);
    }
    gpuCmdBufOffset = off + n;
    return gpuCmdBuf + off;
}

/* n precomputed words; returns where they landed (for the per-draw patches). */
__attribute__((noinline)) uint32_t* NrrCoreDxPut(const uint32_t* w, uint32_t n) {
    uint32_t* p = NrrCoreDxRes(n);
    std::memcpy(p, w, n * sizeof(uint32_t));
    return p;
}

/* One single-register packet. */
__attribute__((noinline)) void NrrCoreDxPut1(uint32_t value, uint32_t header) {
    uint32_t* p = NrrCoreDxRes(2);
    p[0] = value;
    p[1] = header;
}

/* Cold builders write into a plain array. */
struct WordWriter {
    uint32_t* out;
    uint32_t n;

    void One(uint32_t reg, uint32_t mask, uint32_t value) {
        out[n++] = value;
        out[n++] = Hdr(reg, mask);
    }

    void Inc(uint32_t reg, const uint32_t* v, uint32_t count) {
        out[n++] = v[0];
        out[n++] = HdrInc(reg, count);
        for (uint32_t i = 1; i < count; i++) {
            out[n++] = v[i];
        }
        if (((count - 1u) & 1u) != 0) {
            out[n++] = 0;
        }
    }
};

/* --------------------------------------------------------------- cold builders ---- */

/* n60tail [native] sky_late, under the stencil mode of c3d_gdx.h (cold: only while it is set).
 * After the effect `fx` was applied (its group put again or not): check it, and write the mode's
 * stencil test + op right behind (GPUREG_STENCIL_TEST, GPUREG_STENCIL_OP), over the effect group's
 * own. Nothing draws in between, so the GPU state at every draw is the override's. An apply is
 * followed by its block's draws, so the check counts as drawn here (no per-draw hook in this
 * emitter); an apply nothing draws with only makes the frame keep its order. */
GDX_COLD __attribute__((noinline)) void DxStencilApply(const C3D_Effect& fx) {
    uint32_t fx4[4] = { fx.alphaTest, fx.stencilMode, fx.stencilOp, fx.depthTest };
    C3Dgdx_StencilCheck(fx.fragOpMode, fx.alphaBlend, static_cast<uint32_t>(fx.clrLogicOp), fx4);
    C3Dgdx_StencilDraw();
    uint32_t* p = NrrCoreDxRes(4);
    p[0] = fx4[1];
    p[1] = HdrInc(GPUREG_FRAGOP_ALPHA_TEST + 1u, 2);
    p[2] = fx4[2];
    p[3] = 0; /* padding: a packet is an even number of words */
}

/* C3Di_EffectBind's packets, in its order (gdx_submit.c DietEffectBind). */
GDX_COLD void BuildEffectWords(WordWriter& w, const C3D_Effect& fx) {
    w.One(GPUREG_DEPTHMAP_ENABLE, 0xF, fx.zBuffer ? 1u : 0u);
    w.One(GPUREG_FACECULLING_CONFIG, 0xF, static_cast<uint32_t>(fx.cullMode) & 0x3u);
    w.Inc(GPUREG_DEPTHMAP_SCALE, &fx.zScale, 2);
    w.Inc(GPUREG_FRAGOP_ALPHA_TEST, &fx.alphaTest, 4);
    w.One(GPUREG_GAS_DELTAZ_DEPTH, 0x8, static_cast<uint32_t>(GPU_MAKEGASDEPTHFUNC((fx.depthTest >> 4) & 7)) << 24);
    w.One(GPUREG_BLEND_COLOR, 0xF, fx.blendClr);
    w.One(GPUREG_BLEND_FUNC, 0xF, fx.alphaBlend);
    w.One(GPUREG_LOGIC_OP, 0xF, static_cast<uint32_t>(fx.clrLogicOp));
    w.One(GPUREG_COLOR_OPERATION, 0x7, fx.fragOpMode);
    w.One(GPUREG_FRAGOP_SHADOW, 0xF, fx.fragOpShadow);
    w.One(GPUREG_EARLYDEPTH_TEST1, 0x1, fx.earlyDepth ? 1u : 0u);
    w.One(GPUREG_EARLYDEPTH_TEST2, 0xF, fx.earlyDepth ? 1u : 0u);
    w.One(GPUREG_EARLYDEPTH_FUNC, 0x1, static_cast<uint32_t>(fx.earlyDepthFunc));
    w.One(GPUREG_EARLYDEPTH_DATA, 0x7, fx.earlyDepthRef);
}

/* C3Di_AttrInfoBind's packets. */
GDX_COLD void BuildAttrWords(DxLayout& L, const C3D_AttrInfo* info) {
    WordWriter w{ L.attr, 0 };
    w.Inc(GPUREG_ATTRIBBUFFERS_FORMAT_LOW, info->flags, 2);
    w.One(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000u | static_cast<uint32_t>(info->attrCount - 1));
    w.One(GPUREG_VSH_NUM_ATTR, 0xF, static_cast<uint32_t>(info->attrCount - 1));
    w.Inc(GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, reinterpret_cast<const uint32_t*>(&info->permutation), 2);
    if (gBk.xfBoolLoc >= 0) {
        w.One(GPUREG_VSH_BOOLUNIFORM, 0xF, 0);
    }
    L.attrCount = w.n;
}

/* C3Di_BufInfoBind's packets for BufInfo_Init + one BufInfo_Add(stride, attribs, perm). */
GDX_COLD void BuildBufWords(DxLayout& L, uint32_t stride) {
    C3D_BufInfo bi;
    BufInfo_Init(&bi);
    bi.buffers[0].offset = 0;
    bi.buffers[0].flags[0] = static_cast<u32>(L.perm & 0xFFFFFFFFu);
    bi.buffers[0].flags[1] = static_cast<u32>(L.perm >> 32) | (stride << 16) | (static_cast<u32>(L.attribs) << 28);
    bi.bufCount = 1;
    WordWriter w{ L.buf, 0 };
    w.One(GPUREG_ATTRIBBUFFERS_LOC, 0xF, bi.base_paddr >> 3);
    w.Inc(GPUREG_ATTRIBBUFFER0_OFFSET, reinterpret_cast<const uint32_t*>(bi.buffers), sizeof(bi.buffers) / sizeof(u32));
    L.stride = stride;
}

/* The draw calls' own packets (stock drawArrays.c / drawElements.c, GPU_TRIANGLES), with the
 * vertex count (word 6) and the index buffer config (elements, word 4) patched per draw. */
GDX_COLD void BuildDrawWords() {
    WordWriter a{ sArraysWords, 0 };
    a.One(GPUREG_PRIMITIVE_CONFIG, 2, GPU_TRIANGLES);
    a.One(GPUREG_RESTART_PRIMITIVE, 0xF, 1);
    a.One(GPUREG_INDEXBUFFER_CONFIG, 0xF, 0x80000000u);
    a.One(GPUREG_NUMVERTICES, 0xF, 0);
    a.One(GPUREG_VERTEX_OFFSET, 0xF, 0);
    a.One(GPUREG_GEOSTAGE_CONFIG2, 1, 1);
    a.One(GPUREG_START_DRAW_FUNC0, 1, 0);
    a.One(GPUREG_DRAWARRAYS, 0xF, 1);
    a.One(GPUREG_START_DRAW_FUNC0, 1, 1);
    a.One(GPUREG_GEOSTAGE_CONFIG2, 1, 0);
    a.One(GPUREG_VTX_FUNC, 0xF, 1);
    WordWriter e{ sElementsWords, 0 };
    e.One(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM);
    e.One(GPUREG_RESTART_PRIMITIVE, 0xF, 1);
    e.One(GPUREG_INDEXBUFFER_CONFIG, 0xF, 0);
    e.One(GPUREG_NUMVERTICES, 0xF, 0);
    e.One(GPUREG_VERTEX_OFFSET, 0xF, 0);
    e.One(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
    e.One(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
    e.One(GPUREG_START_DRAW_FUNC0, 1, 0);
    e.One(GPUREG_DRAWELEMENTS, 0xF, 1);
    e.One(GPUREG_START_DRAW_FUNC0, 1, 1);
    e.One(GPUREG_GEOSTAGE_CONFIG, 2, 0);
    e.One(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
    e.One(GPUREG_VTX_FUNC, 0xF, 1);
    e.One(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    e.One(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
}

/* C3D_SetViewport(0, 0, 240, 400)'s words, produced by the real setter on citro3d's context and
 * copied out (the context and its flags are put back exactly). */
GDX_COLD void BuildViewportWords() {
    C3D_Context* ctx = C3Di_GetContext();
    u32 savedViewport[5];
    u32 savedScissor[3];
    std::memcpy(savedViewport, ctx->viewport, sizeof(savedViewport));
    std::memcpy(savedScissor, ctx->scissor, sizeof(savedScissor));
    const u32 savedFlags = ctx->flags;
    C3D_SetViewport(0u, 0u, 240u, 400u);
    WordWriter w{ sViewportWords, 0 };
    w.Inc(GPUREG_VIEWPORT_WIDTH, ctx->viewport, 4);
    w.One(GPUREG_VIEWPORT_XY, 0xF, ctx->viewport[4]);
    const u32 scissorOff[3] = { GPU_SCISSOR_DISABLE, 0, 0 };
    w.Inc(GPUREG_SCISSORTEST_MODE, scissorOff, 3);
    std::memcpy(ctx->viewport, savedViewport, sizeof(savedViewport));
    std::memcpy(ctx->scissor, savedScissor, sizeof(savedScissor));
    ctx->flags = savedFlags;
}

/* The headers of the per-use templates. */
GDX_COLD void BuildTemplates() {
    WordWriter e{ sEnvBufWords, 0 };
    e.One(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, 0);
    e.One(GPUREG_TEXENV_BUFFER_COLOR, 0xF, 0);
    e.One(GPUREG_FOG_COLOR, 0xF, 0);
    std::memset(sProjWords, 0, sizeof(sProjWords));
    sProjWords[1] = Hdr(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF);
    sProjWords[3] = HdrPort(GPUREG_VSH_FLOATUNIFORM_DATA, 16);
    static const uint16_t kBorderReg[2] = { GPUREG_TEXUNIT0_BORDER_COLOR, GPUREG_TEXUNIT1_BORDER_COLOR };
    static const uint16_t kTypeReg[2] = { GPUREG_TEXUNIT0_TYPE, GPUREG_TEXUNIT1_TYPE };
    for (int u = 0; u < 2; u++) {
        const u32 zero[5] = { 0, 0, 0, 0, 0 };
        WordWriter t{ sTexWords[u], 0 };
        t.Inc(kBorderReg[u], zero, 5);
        t.One(kTypeReg[u], 0xF, 0);
    }
    WordWriter st{ sTexStatusWords, 0 };
    st.One(GPUREG_TEXUNIT_CONFIG, 0xB, 0);
    st.One(GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
    st.One(GPUREG_TEXUNIT0_SHADOW, 0xF, 0);
}

GDX_COLD void BuildLayout(int prog, const void* attr, uint32_t stride, int attribs, uint64_t perm) {
    DxLayout& L = sLayout[prog];
    BuildAttrWords(L, static_cast<const C3D_AttrInfo*>(attr));
    L.attribs = attribs;
    L.perm = perm;
    BuildBufWords(L, stride);
}

/* The content id of one group's words (cold, NrrStateGet / BuildInto). */
GDX_COLD uint16_t InternGroup(int g, const uint32_t* w) {
    std::vector<std::vector<uint32_t>>& seen = sGroupSeen[g];
    for (size_t i = 0; i < seen.size(); i++) {
        if (std::memcmp(seen[i].data(), w, GroupLen(g) * sizeof(uint32_t)) == 0) {
            return static_cast<uint16_t>(i);
        }
    }
    if (seen.size() >= 0xFFFEu) {
        return 0xFFFEu; /* a full table: this content is never "held" (always re-sent) */
    }
    seen.emplace_back(w, w + GroupLen(g));
    return static_cast<uint16_t>(seen.size() - 1);
}

/* A cube texture (the backend binds none): citro3d's own bind writes the words. */
GDX_COLD void DxTexCube(int u, const C3D_Tex* src, uint32_t param) {
    C3D_Tex copy = *src;
    copy.param = param;
    C3Di_SetTex(u, &copy);
}

/* What the next citro3d draw would have emitted first, emitted now (the first few are named in
 * the log: context flags pending, VS / GS uniforms pending). */
GDX_COLD void DxSyncCold() {
    static int sLogged = 0;
    if (sLogged < 12) {
        sLogged++;
        const C3D_Context* ctx = C3Di_GetContext();
        gdx3ds_nrr_log("[nrrdx] sync #%d flags=0x%08lx unif vs=%d gs=%d texEnvBuf=0x%lx", sLogged,
                       (unsigned long)ctx->flags, C3Dgdx_UniformsPending(GPU_VERTEX_SHADER),
                       C3Dgdx_UniformsPending(GPU_GEOMETRY_SHADER), (unsigned long)ctx->texEnvBuf);
    }
    NRR_CRUMB_FIRST(NRR_CRUMB_DXSYNC, 4, "dx sync > C3Dgdx_FlushState flags=0x%08lx cmdbuf off=%lu/%lu",
                    (unsigned long)C3Di_GetContext()->flags, (unsigned long)gpuCmdBufOffset, (unsigned long)gpuCmdBufSize);
    C3Dgdx_FlushState();
    gDxStats.syncs++;
}

/* Every C3D_Context flag C3Di_UpdateContext acts on, less the fog LUT (gdx_submit.c
 * C3DGDX_CTX_WORK_FLAGS; C3Dgdx_StatePending decides the LUT): the cheap mid-hand-off test. */
constexpr u32 kCtxWorkFlags = C3DiF_FrameBuf | C3DiF_Viewport | C3DiF_Scissor | C3DiF_Program | C3DiF_AttrInfo |
                              C3DiF_BufInfo | C3DiF_Effect | C3DiF_TexAll | C3DiF_TexStatus | C3DiF_ProcTex |
                              C3DiF_ProcTexColorLut | C3DiF_ProcTexLutAll | C3DiF_TexEnvBuf | C3DiF_TexEnvAll |
                              C3DiF_LightEnv;

/* n60draw: the uniform registers a citro3d flush is about to write (its dirty marks), so what the
 * direct words believe of them can follow (the gpuxf block, the projection, the stereo row, the
 * bool). Before n60draw a mid-hand-off flush that carried uniforms left sDx.xf believing the
 * GPU held NRR's block; no category does that today ([nrrdx] midsyncs are viewport/scissor). */
struct DxUnifDirt {
    uint8_t xf[kXfVec4s];
    uint8_t proj;
    uint8_t stk;
    uint8_t bools;
    uint8_t any;
};

GDX_COLD void DxUnifDirtOf(DxUnifDirt& d) {
    std::memset(&d, 0, sizeof(d));
    if (!C3Dgdx_UniformsPending(0)) {
        return;
    }
    d.any = 1;
    if (gBk.xfUnifLoc >= 0) {
        std::memcpy(d.xf, &C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.xfUnifLoc], kXfVec4s);
    }
    for (int i = 0; i < 4; i++) {
        d.proj |= C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.projLoc + i] ? 1u : 0u;
    }
    d.stk = gBk.stkLoc >= 0 && C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.stkLoc] ? 1u : 0u;
    d.bools = C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] ? 1u : 0u;
}

/* After the flush: the registers it wrote now hold citro3d's values. */
GDX_COLD void DxUnifDirtApply(const DxUnifDirt& d) {
    if (!d.any) {
        return;
    }
    for (int i = 0; i < kXfVec4s; i++) {
        if (d.xf[i]) {
            sDx.xf[i] = C3D_FVUnif[GPU_VERTEX_SHADER][gBk.xfUnifLoc + i];
        }
    }
    sDx.xfHint = 0;
    if (d.proj) {
        sDx.proj = 0;
    }
    if (d.stk) {
        sDx.stk = C3D_FVUnif[GPU_VERTEX_SHADER][gBk.stkLoc];
    }
    if (d.bools) {
        sDx.attr = -1; /* the xfon bool rides the attribute words */
    }
}

/* State changed through citro3d INSIDE a hand-off (a category calling the backend, e.g. n60mach's
 * Interp::ApplyViewportScissor under keepVp): the citro3d path emits it at its next draw, on top of
 * what NRR set before and under what NRR sets after. Here: emit it now, and forget what the direct
 * words believed of the groups it rewrote, so the next apply re-sends them where citro3d's path
 * would re-set them from the context. */
GDX_COLD void DxMidSync() {
    const u32 f = C3Di_GetContext()->flags;
    if (!C3Dgdx_StatePending()) {
        C3Dgdx_unifTouch = 0; /* n60draw: nothing pending, uniforms included */
        return;
    }
    DxUnifDirt dirt;
    if (gDraw != 0) {
        DxUnifDirtOf(dirt);
    }
    C3Dgdx_FlushState();
    C3Dgdx_unifTouch = 0;
    gDxStats.midSyncs++;
    if (gDraw != 0) {
        DxUnifDirtApply(dirt);
    }
    if (f & (C3DiF_Viewport | C3DiF_Scissor)) {
        sDx.group[0] = 0xFFFFu;
    }
    if (f & C3DiF_Effect) {
        sDx.group[1] = 0xFFFFu;
    }
    for (int i = 0; i < kStages; i++) {
        if (f & C3DiF_TexEnv(i)) {
            sDx.group[2 + i] = 0xFFFFu;
        }
    }
    if (f & C3DiF_AttrInfo) {
        sDx.attr = -1;
    }
    if (f & C3DiF_BufInfo) {
        sDx.bufLayout = -1;
    }
    for (int u = 0; u < 2; u++) {
        if (f & C3DiF_Tex(u)) {
            sDx.texSrc[u] = nullptr;
        }
    }
}

/* Before words that state set through citro3d could overtake (apply, constant): the flag test.
 * nrrdiet: plain noinline -- 2 call sites (NrrCoreDxApply, NrrCoreDxConst), both counted; `inline`
 * had this duplicated at both instead of one shared, uncounted ("DxContextGuard" matches no
 * nrr_size.sh pattern) copy. */
__attribute__((noinline)) void DxContextGuard() {
    if ((C3Di_GetContext()->flags & kCtxWorkFlags) != 0) {
        DxMidSync();
    }
}

/* n60draw: nothing a citro3d draw would emit first is pending, read from the context flags and the
 * uniform-touch hint instead of the dirty-array scan. Anything that MAY be pending (any work flag,
 * the fog LUT, a uniform write since the last clear, fixed attributes, lights, gas) falls through to
 * the exact test. */
inline bool DxQuietFast() {
    const C3D_Context* ctx = C3Di_GetContext();
    const u32 fog = ctx->texEnvBuf & 7u;
    const u32 lut = fog != GPU_NO_FOG ? static_cast<u32>(C3DiF_FogLut) : 0u; /* the LUT stays dirty while fog is off */
    return ((ctx->flags & (kCtxWorkFlags | lut)) | C3Dgdx_unifTouch | ctx->fixedAttribDirty) == 0 &&
           ctx->lightEnv == nullptr && fog != GPU_GAS;
}

/* Before a draw: everything citro3d's draw would emit first, uniforms included. nrrdiet: plain
 * noinline -- 2 call sites (NrrCoreDxDraw2D, NrrCoreDxDrawXf), same dedup as DxContextGuard. */
__attribute__((noinline)) void DxDrawGuard() {
    if ((gDraw & kDrawGuard) != 0) {
        if (DxQuietFast()) {
            sDrawSt.guardFast++;
            return;
        }
        sDrawSt.guardFull++;
    }
    if (C3Dgdx_StatePending()) {
        DxMidSync();
    } else {
        C3Dgdx_unifTouch = 0;
    }
}

/* ------------------------------------------------------------------- hot path ---- */

/* n60draw carry, keepVp: citro3d's viewport / scissor, as the begin flush would have sent them. */
GDX_COLD void DxLazyVp() {
    const C3D_Context* ctx = C3Di_GetContext();
    uint32_t w[12];
    WordWriter ww{ w, 0 };
    if (sDx.lazy & C3DiF_Viewport) {
        ww.Inc(GPUREG_VIEWPORT_WIDTH, ctx->viewport, 4);
        ww.One(GPUREG_VIEWPORT_XY, 0xF, ctx->viewport[4]);
    }
    if (sDx.lazy & C3DiF_Scissor) {
        ww.Inc(GPUREG_SCISSORTEST_MODE, ctx->scissor, 3);
        sDx.group[0] = 0xFFFFu;
    }
    NrrCoreDxPut(w, ww.n);
    sDrawSt.lazyVp++;
}

/* The first draw-state words of a hand-off: the full main target (unless the category keeps the
 * walk's viewport), the scissor off, the fog unit off -- NrrCoreC3dFirstTouch's state. */
void NrrCoreDxFirstTouch(const NrrCtx* c) {
    C3D_Context* ctx = C3Di_GetContext();
    if (!c->keepVp) {
        uint32_t* v = NrrCoreDxPut(sViewportWords, 12);
        v[10] = ctx->scissor[1];
        v[11] = ctx->scissor[2];
        sDx.touched |= G_VP | G_SCISSOR;
        sDx.group[0] = 0xFFFFu; /* the scissor no longer holds a block's words */
    } else if (GDX_UNLIKELY((sDx.lazy & (C3DiF_Viewport | C3DiF_Scissor)) != 0)) {
        DxLazyVp();
    }
    sDx.lazy &= ~static_cast<uint32_t>(C3DiF_Viewport | C3DiF_Scissor);
    uint32_t* p = NrrCoreDxPut(sEnvBufWords, 6);
    p[0] = (ctx->texEnvBuf & ~0x100FFu) | (GPU_NO_FOG & 7u) | ((GPU_PLAIN_DENSITY & 1u) << 3);
    p[2] = ctx->texEnvBufClr;
    p[4] = ctx->fogClr;
    sDx.touched |= G_ENVBUF;
    sDx.firstDone = 1;
}

/* nrrdiet: plain noinline -- 3 call sites (NrrCoreDxApply, NrrCoreDxDraw2D, NrrCoreDxDrawXf), same
 * dedup as DxContextGuard. */
__attribute__((noinline)) void DxTouch(const NrrCtx* c) {
    if (!sDx.firstDone) {
        NrrCoreDxFirstTouch(c);
    }
}

/* n60draw carry, lazy: effect + stages when no block was applied yet (cold: every category applies first). */
GDX_COLD void DxLazyState() {
    const C3D_Context* ctx = C3Di_GetContext();
    uint32_t w[kDxStateWords];
    WordWriter ww{ w, 0 };
    if (sDx.lazy & C3DiF_Effect) {
        BuildEffectWords(ww, ctx->effect);
        if (GDX_UNLIKELY(C3Dgdx_stencilMode != 0)) {
            uint32_t fx4[4] = { ctx->effect.alphaTest, ctx->effect.stencilMode, ctx->effect.stencilOp,
                                ctx->effect.depthTest };
            C3Dgdx_StencilCheck(ctx->effect.fragOpMode, ctx->effect.alphaBlend,
                                static_cast<uint32_t>(ctx->effect.clrLogicOp), fx4);
            C3Dgdx_StencilDraw(); /* the lazy state goes out right before a draw */
            w[10] = fx4[1]; /* n60tail sky_late: the effect's packets are w[0..33]; stencil test, op */
            w[11] = fx4[2];
        }
        sDx.group[1] = 0xFFFFu;
    }
    for (int i = 0; i < kStages; i++) {
        if (sDx.lazy & C3DiF_TexEnv(i)) {
            ww.Inc(TexEnvReg(i), reinterpret_cast<const uint32_t*>(&ctx->texEnv[i]), sizeof(C3D_TexEnv) / sizeof(uint32_t));
            sDx.group[2 + i] = 0xFFFFu;
            sDx.stageConst[i] = ctx->texEnv[i].color;
        }
    }
    NrrCoreDxPut(w, ww.n);
    sDrawSt.lazyState++;
}

/* The texture units NRR has not bound and, when it bound none, the status (hot under the carry:
 * citro3d's textures are pending after every hand-off that bound one). */
__attribute__((noinline)) void DxLazyTex() {
    const C3D_Context* ctx = C3Di_GetContext();
    uint32_t units = 0;
    for (int i = 0; i < 3; i++) {
        const C3D_Tex* t = ctx->tex[i];
        if (t == nullptr) {
            continue;
        }
        units |= 1u << i;
        if ((sDx.deferred & C3DiF_Tex(i)) == 0 || (sDx.texBound & (1u << i)) != 0) {
            continue;
        }
        if (i < 2 && C3Di_TexIs2D(const_cast<C3D_Tex*>(t))) {
            uint32_t* p = NrrCoreDxPut(sTexWords[i], 8);
            p[0] = t->border;
            p[2] = t->dim;
            p[3] = t->param;
            p[4] = t->lodParam;
            p[5] = osConvertVirtToPhys(t->data) >> 3;
            p[6] = static_cast<uint32_t>(t->fmt);
        } else {
            C3Di_SetTex(i, const_cast<C3D_Tex*>(t));
        }
    }
    if (sDx.texBound == 0) {
        const uint32_t cfg = (sDx.lazy & C3DiF_TexAll) ? ((sDx.lazyTexConfig & ~7u) | units | BIT(16)) : sDx.lazyTexConfig;
        NrrCoreDxPut1(cfg, Hdr(GPUREG_TEXUNIT_CONFIG, 0xB));
        if (cfg & BIT(16)) {
            NrrCoreDxPut1(BIT(16), Hdr(GPUREG_TEXUNIT_CONFIG, 0x4));
        }
        NrrCoreDxPut1(ctx->texShadow, Hdr(GPUREG_TEXUNIT0_SHADOW, 0xF));
    }
    sDrawSt.lazyTex++;
}

/* n60draw carry: before the first draw, what citro3d's begin flush would have put on the GPU and
 * NRR's own words have not overwritten. */
__attribute__((noinline)) void DxLazy() {
    if (GDX_UNLIKELY((sDx.lazy & (C3DiF_Effect | C3DiF_TexEnvAll)) != 0)) {
        DxLazyState();
    }
    if (sDx.lazy & (C3DiF_TexAll | C3DiF_TexStatus)) {
        DxLazyTex();
    }
    sDx.lazy = 0;
}

/* nrrdiet: plain noinline -- 2 call sites (NrrCoreDxDraw2D, NrrCoreDxDrawXf), same dedup as
 * DxContextGuard. */
__attribute__((noinline)) void DxLazyDraw() {
    if (GDX_UNLIKELY(sDx.lazy != 0)) {
        DxLazy();
    }
}

/* One texture unit (the citro3d path's NrrCoreC3dTexUnit decision: rebind only when the source,
 * its data or the param bits changed). Returns the unit's bit when it emitted. */
uint32_t NrrCoreDxTexUnit(int u, const NrrTexUnit& t) {
    const C3D_Tex* src = static_cast<const C3D_Tex*>(t.obj);
    if (src == nullptr) {
        NrrBkTex bt;
        if (t.id == NRR_TEX_NONE || !gdx3ds_nrr_bk_tex(t.id, &bt)) {
            return 0;
        }
        src = static_cast<const C3D_Tex*>(bt.tex);
    }
    const uint32_t param = (src->param & ~t.mask) | t.bits;
    if (sDx.texSrc[u] == src && sDx.texData[u] == src->data && sDx.texParam[u] == param) {
        return 0;
    }
    sDx.texSrc[u] = src;
    sDx.texData[u] = src->data;
    sDx.texParam[u] = param;
    if (!C3Di_TexIs2D(const_cast<C3D_Tex*>(src))) {
        DxTexCube(u, src, param);
        return 1u << u;
    }
    uint32_t* p = NrrCoreDxPut(sTexWords[u], 8);
    p[0] = src->border;
    p[2] = src->dim;
    p[3] = param;
    p[4] = src->lodParam;
    p[5] = osConvertVirtToPhys(src->data) >> 3;
    p[6] = static_cast<uint32_t>(src->fmt);
    return 1u << u;
}

/* After a bind: the unit enables and the texture cache clear (C3Di_UpdateContext's TexStatus). */
void NrrCoreDxTexStatus(uint32_t bound) {
    C3D_Context* ctx = C3Di_GetContext();
    uint32_t units = sDx.texBound | bound;
    sDx.texBound = static_cast<uint8_t>(units);
    sDx.touched |= G_TEX;
    for (int i = 0; i < 3; i++) {
        units |= ctx->tex[i] != nullptr ? (1u << i) : 0u;
    }
    uint32_t* p = NrrCoreDxPut(sTexStatusWords, 6);
    p[0] = (ctx->texConfig & ~7u) | units | BIT(16);
    p[4] = ctx->texShadow;
}

/* The attribute layout and the xfon bool (NrrCoreC3dAttr's state). */
void NrrCoreDxAttr(int prog) {
    const DxLayout& L = sLayout[prog];
    uint32_t* p = NrrCoreDxPut(L.attr, L.attrCount);
    sDx.attr = static_cast<int8_t>(prog);
    sDx.touched |= G_ATTR;
    if (L.attrCount > kAttrWords) {
        const uint32_t bit = 1u << (gBk.xfBoolLoc - 0x68);
        const uint32_t bools = C3D_BoolUnifs[GPU_VERTEX_SHADER] & ~bit;
        p[kAttrWords] = 0x7FFF0000u | bools | (prog != NRR_PROG_2D ? bit : 0u);
        sDx.touched |= G_BOOL;
    }
}

/* A 4x4 matrix into the projection uniform (C3D_FVUnifMtx4x4's register image: row i -> loc + i). */
void NrrCoreDxProj(const void* mtx, uint8_t kind) {
    const uint32_t* m = static_cast<const uint32_t*>(mtx);
    uint32_t* p = NrrCoreDxPut(sProjWords, 20);
    p[0] = 0x80000000u | static_cast<uint32_t>(gBk.projLoc);
    p[2] = m[0];
    std::memcpy(p + 4, m + 1, 15u * sizeof(uint32_t));
    sDx.proj = kind;
    sDx.touched |= G_PROJ;
}

/* Buffer 0 at `verts`: one offset write while the layout stands, the full config when it changes
 * (NrrCoreC3dBuf: BufInfo_Init + BufInfo_Add). */
void NrrCoreDxBuf(const void* verts, int layout, uint32_t stride) {
    const uint32_t off = osConvertVirtToPhys(verts) - kBufferBasePaddr;
    if (sDx.bufLayout == layout && sDx.bufStride == stride) {
        if (off != sDx.bufOff) {
            NrrCoreDxPut1(off, Hdr(GPUREG_ATTRIBBUFFER0_OFFSET, 0xF));
            sDx.bufOff = off;
        }
        return;
    }
    DxLayout& L = sLayout[layout];
    if (L.stride != stride) {
        BuildBufWords(L, stride);
    }
    NrrCoreDxPut(L.buf, kBufWords)[2] = off;
    sDx.bufLayout = static_cast<int8_t>(layout);
    sDx.bufStride = stride;
    sDx.bufOff = off;
    sDx.touched |= G_BUF;
}

typedef uint32_t __attribute__((may_alias)) AliasWord;

/* citro3d's float compare of one vec4 (NrrCoreC3dDrawXf: cur.x == v[0] ...), for the rare words
 * whose bits differ but whose floats may still compare equal (+0 / -0). */
GDX_COLD bool DxFloatEqual(const C3D_FVec& cur, const float* v) {
    return cur.x == v[0] && cur.y == v[1] && cur.z == v[2] && cur.w == v[3];
}

/* One gpuxf row against what the GPU holds: equal bits skip; different bits go to citro3d's float
 * compare (+0 / -0 skip there); else the row is taken. True = the row must be sent. nrrdiet: plain
 * noinline -- called from NrrCoreDxXfRows's loop AND from NrrCoreDxXfUniforms's own fallback loop
 * (inlined whole into NrrCoreDxDrawXf, both counted); `inline` had this duplicated across both
 * instead of one shared, uncounted copy. */
__attribute__((noinline)) bool DxXfRowTake(int i, const AliasWord* in) {
    AliasWord* cur = reinterpret_cast<AliasWord*>(&sDx.xf[i]);
    if (cur[0] == in[3] && cur[1] == in[2] && cur[2] == in[1] && cur[3] == in[0]) {
        return false;
    }
    if (DxFloatEqual(sDx.xf[i], reinterpret_cast<const float*>(in))) {
        return false;
    }
    cur[0] = in[3];
    cur[1] = in[2];
    cur[2] = in[1];
    cur[3] = in[0];
    return true;
}

/* n60draw: the rows of `rows` only; returns the last row taken (-1 none), *lo the first. */
__attribute__((noinline)) int NrrCoreDxXfRows(const AliasWord* in, uint32_t rows, int* lo) {
    int first = -1;
    int last = -1;
    sDrawSt.xfHinted++;
    while (rows != 0) {
        const int i = __builtin_ctz(rows);
        rows &= rows - 1u;
        sDrawSt.xfRows++;
        if (!DxXfRowTake(i, in + 4 * i)) {
            continue;
        }
        first = first < 0 ? i : first;
        last = i;
    }
    *lo = first;
    return last;
}

/* The gpuxf block: the vec4s that differ from what the GPU holds, sent as ONE run from the first
 * to the last changed register. The decision is citro3d's float compare (NrrCoreC3dDrawXf): equal
 * bits skip (a NaN citro3d would re-send carries the same bits: the GPU holds them already);
 * different bits go to the float compare (+0 / -0 skip there, as in citro3d). C3D_FVec stores
 * (w, z, y, x): word k of the register is v[3 - k].
 * n60draw ([native] nrr_draw bit 2): `rows` = the rows the caller may have changed since its
 * previous block of this hand-off; once this hand-off compared a block, only those are compared
 * (every other row equals the previous block, which the GPU holds or float-equals). */
void NrrCoreDxXfUniforms(const float* unif, uint32_t rows) {
    if (!sDx.xfSeeded) {
        std::memcpy(sDx.xf, &C3D_FVUnif[GPU_VERTEX_SHADER][gBk.xfUnifLoc], sizeof(sDx.xf));
        sDx.xfSeeded = 1;
    }
    const AliasWord* in = reinterpret_cast<const AliasWord*>(unif);
    int lo = -1;
    int hi = -1;
    if (sDx.xfHint && (gDraw & kDrawXfRows) != 0 && rows != kXfAllRows) {
        hi = NrrCoreDxXfRows(in, rows, &lo);
    } else {
        for (int i = 0; i < kXfVec4s; i++, in += 4) {
            if (!DxXfRowTake(i, in)) {
                continue;
            }
            lo = lo < 0 ? i : lo;
            hi = i;
        }
    }
    sDx.xfHint = 1;
    if (lo < 0) {
        return;
    }
    const uint32_t n = static_cast<uint32_t>(hi - lo + 1) * 4u;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(&sDx.xf[lo]);
    uint32_t* p = NrrCoreDxRes(n + 4u);
    p[0] = 0x80000000u | static_cast<uint32_t>(gBk.xfUnifLoc + lo);
    p[1] = Hdr(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF);
    p[2] = w[0];
    p[3] = HdrPort(GPUREG_VSH_FLOATUNIFORM_DATA, n);
    std::memcpy(p + 4, w + 1, (n - 1u) * sizeof(uint32_t));
    p[n + 3u] = 0;
    sDx.touched |= G_XF;
}

/* n60stereo: the draw's stereo row (frame replay only), sent when its bits differ from what the GPU
 * holds -- NrrCoreC3dStereo's float compare decides, as for the gpuxf block. */
__attribute__((noinline)) void NrrCoreDxStereo(NrrCtx* c, const float* verts2d) {
    const float* row = NrrStereoRow(c, verts2d);
    if (row == nullptr) {
        return;
    }
    if (!sDx.stkSeeded) {
        sDx.stk = C3D_FVUnif[GPU_VERTEX_SHADER][gBk.stkLoc];
        sDx.stkSeeded = 1;
    }
    if (sDx.stk.x == row[0] && sDx.stk.y == row[1] && sDx.stk.z == row[2] && sDx.stk.w == row[3]) {
        return;
    }
    sDx.stk.x = row[0];
    sDx.stk.y = row[1];
    sDx.stk.z = row[2];
    sDx.stk.w = row[3];
    const uint32_t* w = reinterpret_cast<const uint32_t*>(&sDx.stk);
    uint32_t* p = NrrCoreDxRes(8);
    p[0] = 0x80000000u | static_cast<uint32_t>(gBk.stkLoc);
    p[1] = Hdr(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF);
    p[2] = w[0];
    p[3] = HdrPort(GPUREG_VSH_FLOATUNIFORM_DATA, 4);
    p[4] = w[1];
    p[5] = w[2];
    p[6] = w[3];
    p[7] = 0;
    sDx.touched |= G_STK;
}

/* Under keepVp the scissor is cut to the game's clip square (NrrCoreC3dProj3d's rule). */
GDX_COLD void DxClipScissor(const C3D_Mtx* m) {
    u32 cut[3];
    if (!NrrClipScissorWords(m, C3Di_GetContext()->scissor, cut)) {
        return;
    }
    uint32_t* p = NrrCoreDxRes(4);
    p[0] = cut[0];
    p[1] = HdrInc(GPUREG_SCISSORTEST_MODE, 3);
    p[2] = cut[1];
    p[3] = cut[2];
    sDx.touched |= G_SCISSOR;
    sDx.group[0] = 0xFFFFu;
}

/* The 3D scene map (gdx3ds_nrr_bk_proj3d: the fixup, the border mode's map, the [vpfix] fold). */
void NrrCoreDxProj3d(const NrrCtx* c) {
    const C3D_Mtx* m = static_cast<const C3D_Mtx*>(gdx3ds_nrr_bk_proj3d());
    NrrCoreDxProj(m, 2);
    if (c->keepVp) {
        DxClipScissor(m);
    }
}

void NrrCoreDxNoteDraw() {
    C3Di_GetContext()->flags |= C3DiF_DrawUsed;
    gDxStats.draws++;
}

/* The uniform registers the direct words wrote go back to citro3d's values on its next upload. */
void NrrCoreDxEndUniforms(uint32_t groups) {
    if (groups & G_PROJ) {
        std::memset(&C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.projLoc], 1, 4);
    }
    if (groups & G_XF) {
        std::memset(&C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.xfUnifLoc], 1, kXfVec4s);
    }
    if (groups & G_BOOL) {
        C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] = true;
    }
    if (groups & G_STK) {
        C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.stkLoc] = true;
    }
    if ((groups & (G_PROJ | G_XF | G_STK)) != 0 && (C3Dgdx_mode & C3DGDX_DIET_ELIDE) != 0) {
        C3Dgdx_InvalidateUniformShadow(); /* c3d_gdx.h: words written behind ELIDE's shadow */
    }
}

} // namespace

/* ------------------------------------------------------------------ public (core) ---- */

GDX_COLD void nrrcore::NrrDxBuildState(NrrState& s) {
    WordWriter w{ s.dx, 0 };
    w.Inc(GPUREG_SCISSORTEST_MODE, s.scissor, 3);
    BuildEffectWords(w, s.effect);
    for (int i = 0; i < kStages; i++) {
        w.Inc(TexEnvReg(i), reinterpret_cast<const uint32_t*>(&s.env[i]), sizeof(C3D_TexEnv) / sizeof(uint32_t));
        s.dxConst[i] = s.env[i].color;
    }
    s.dxCount = w.n;
    if (w.n != kDxStateWords) {
        svcBreak(USERBREAK_PANIC); /* kGroupOff / kGroupLen describe a 74-word block */
    }
    for (int g = 0; g < kDxGroups; g++) {
        s.dxGroup[g] = InternGroup(g, s.dx + kGroupOff[g]);
    }
}

/* n60intro: only the scissor changed (NrrStateRescissor): its group's words and content id. */
GDX_COLD void nrrcore::NrrDxRescissor(NrrState& s) {
    WordWriter w{ s.dx, 0 };
    w.Inc(GPUREG_SCISSORTEST_MODE, s.scissor, 3);
    if (w.n != GroupLen(0)) {
        svcBreak(USERBREAK_PANIC); /* the scissor group is the block's first 4 words */
    }
    s.dxGroup[0] = InternGroup(0, s.dx);
}

GDX_COLD void nrrcore::NrrDxLatch() {
    gDraw = gdx3ds_native_raw != nullptr ? static_cast<uint32_t>(gdx3ds_native_raw("nrr_draw", 0)) & kDrawAll : 0u;
    gdx3ds_nrr_log("[nrrdraw] init bits=0x%lx (guard=%d xfrows=%d carry=%d trkpair=%d) direct=%d", (unsigned long)gDraw,
                   (gDraw & kDrawGuard) ? 1 : 0, (gDraw & kDrawXfRows) ? 1 : 0, (gDraw & kDrawCarry) ? 1 : 0,
                   (gDraw & kDrawTrkPair) ? 1 : 0, gDirect);
    BuildViewportWords();
    BuildTemplates();
    BuildDrawWords();
    BuildLayout(NRR_PROG_2D, gBk.attrLegacy, GDX3DS_VBOPACK_STRIDE * sizeof(float), 4, 0x3210);
    BuildLayout(NRR_PROG_XF, gBk.attrXf, 16, 3, 0x210);
    BuildLayout(NRR_PROG_XF_F, gBk.attrXfF, 24, 3, 0x210);
}

namespace {

/* n60draw carry: the previous hand-off's end is where the command stream still stands, and what is
 * pending in citro3d is only what the direct words track or re-emit themselves (kCarryFlags). No
 * state changes. *defer = the pending flags. */
__attribute__((noinline)) bool DxCarryable(u32* defer) {
    const C3D_Context* ctx = C3Di_GetContext();
    if (!sMark.valid || gpuCmdBuf != sMark.buf || gpuCmdBufOffset != sMark.off ||
        ctx->gxQueue.numEntries != sMark.entries || sEpoch != sMark.epoch) {
        return false;
    }
    sDrawSt.adjacent++;
    if (C3Dgdx_unifTouch != 0) {
        sDrawSt.refUnif++; /* a uniform write since the end: citro3d's own rows would be lost */
        return false;
    }
    u32 f = ctx->flags & (kCtxWorkFlags | C3DiF_FogLut);
    if ((ctx->texEnvBuf & 7u) == GPU_NO_FOG) {
        f &= ~static_cast<u32>(C3DiF_FogLut); /* stays dirty while fog is off (C3Dgdx_StatePending) */
    }
    if ((f & ~kCarryFlags) != 0 || ctx->fixedAttribDirty != 0 || ctx->lightEnv != nullptr ||
        (ctx->texEnvBuf & 7u) == GPU_GAS) {
        sDrawSt.refFlags++;
        return false;
    }
    /* Under the verify the context is the citro3d path's: the direct model's own end marks join. */
    *defer = f | (gDirect == 2 ? (sMark.endFlags & kCarryFlags) : 0u);
    return true;
}

/* The uniform rows the previous end marked dirty (only those: the touch hint says nobody wrote a
 * uniform since) stay unsent: the GPU holds NRR's values and sDx knows them. NrrCoreDxEnd marks
 * them again (the touched groups carry over). */
void DxCarryUnifClear(uint32_t groups) {
    if (groups & G_PROJ) {
        std::memset(&C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.projLoc], 0, 4);
    }
    if (groups & G_XF) {
        std::memset(&C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.xfUnifLoc], 0, kXfVec4s);
    }
    if (groups & G_BOOL) {
        C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] = false;
    }
    if (groups & G_STK) {
        C3D_FVUnifDirty[GPU_VERTEX_SHADER][gBk.stkLoc] = false;
    }
}

/* A carried begin: keep sDx (attributes, buffer, groups, stage constants, the gpuxf block and stereo
 * row), leave citro3d's pending groups for NrrCoreDxEnd to re-mark (deferred), and emit lazily what
 * the begin flush would have put on the GPU under NRR's own words (lazy). Under the verify the
 * citro3d path is the real one: it gets its flush, and the verify's direct model keeps its GPU. */
GDX_COLD void DxCarryBeginVerify() {
    const u32* buf = gpuCmdBuf;
    const u32 off = gpuCmdBufOffset;
    if (C3Dgdx_StatePending()) {
        DxSyncCold();
    }
    C3Dgdx_unifTouch = 0;
    NrrDxVerifyBegin(1, buf + off, gpuCmdBufOffset - off);
}

__attribute__((noinline)) void DxCarryBegin(u32 defer) {
    C3D_Context* ctx = C3Di_GetContext();
    sDrawSt.carried++;
    sDx.deferred = defer;
    sDx.lazy = defer & kLazyFlags;
    sDx.lazyTexConfig = ctx->texConfig;
    if (gDirect == 2) {
        DxCarryBeginVerify();
        return;
    }
    ctx->flags &= ~defer;
    DxCarryUnifClear(sDx.touched);
}

/* Where this hand-off's words end (n60draw carry): valid only when no uniform was written behind
 * the hint since the last clear (NrrCoreDxEnd's own dirty marks are not writes). */
__attribute__((noinline)) void DxMarkEnd(u32 endFlags) {
    if ((gDraw & kDrawCarry) == 0 || C3Dgdx_unifTouch != 0) {
        sMark.valid = false;
        return;
    }
    sMark.buf = gpuCmdBuf;
    sMark.off = gpuCmdBufOffset;
    sMark.entries = C3Di_GetContext()->gxQueue.numEntries;
    sMark.epoch = sEpoch;
    sMark.endFlags = endFlags;
    sMark.valid = true;
}

} // namespace

void nrrcore::NrrCoreDxProjInvalidate() {
    sDx.proj = 0;
}

void nrrcore::NrrCoreDxBegin() {
    gDxStats.handoffs++;
    sDx.proj = 0;
    sDx.texBound = 0;
    sDx.texSrc[0] = nullptr;
    sDx.texSrc[1] = nullptr;
    sDx.firstDone = 0;
    sDx.xfHint = 0;
    u32 defer = 0;
    const bool carry = (gDraw & kDrawCarry) != 0 && DxCarryable(&defer);
    sMark.valid = false;
    if (carry) {
        DxCarryBegin(defer);
        return;
    }
    sDx.touched = 0;
    sDx.attr = -1;
    sDx.bufLayout = -1;
    sDx.xfSeeded = 0;
    sDx.stkSeeded = 0;
    sDx.deferred = 0;
    sDx.lazy = 0;
    std::memset(sDx.stageConst, 0xFF, sizeof(sDx.stageConst)); /* unknown: the first write always goes out */
    std::memset(sDx.group, 0xFF, sizeof(sDx.group));
    if (C3Dgdx_StatePending()) {
        DxSyncCold();
    }
    C3Dgdx_unifTouch = 0; /* n60draw: nothing is pending now */
    if (gDirect == 2) {
        NrrDxVerifyBegin(0, nullptr, 0);
    }
}

void nrrcore::NrrCoreDxTaskEnd() {
    sEpoch++;
    sMark.valid = false;
}

void nrrcore::NrrCoreDxMarkEnd() {
    uint32_t unif = 0;
    DxMarkEnd(NrrDxEndFlags(&unif));
}

void nrrcore::NrrCoreDxApply(NrrCtx* c, const NrrState* s) {
    DxContextGuard();
    DxTouch(c);
    int g = c->keepVp ? 1 : 0; /* keepVp: the walk's scissor stays */
    while (g < kDxGroups) {
        if (s->dxGroup[g] == sDx.group[g] && s->dxGroup[g] != 0xFFFEu) {
            g++;
            continue;
        }
        int e = g;
        while (e < kDxGroups && (s->dxGroup[e] != sDx.group[e] || s->dxGroup[e] == 0xFFFEu)) {
            sDx.group[e] = s->dxGroup[e];
            e++;
        }
        NrrCoreDxPut(s->dx + kGroupOff[g], kGroupOff[e] - kGroupOff[g]); /* one copy per changed run */
        g = e;
    }
    if (GDX_UNLIKELY(C3Dgdx_stencilMode != 0)) {
        DxStencilApply(s->effect); /* n60tail sky_late: checked, and its stencil words after the block's */
    }
    std::memcpy(sDx.stageConst, s->dxConst, sizeof(sDx.stageConst));
    sDx.touched |= G_SCISSOR | G_EFFECT | G_TEXENV;
    sDx.lazy &= ~static_cast<uint32_t>(C3DiF_Effect | C3DiF_TexEnvAll); /* n60draw carry: the block's are on */
    const uint32_t bound = NrrCoreDxTexUnit(0, s->tex[0]) | NrrCoreDxTexUnit(1, s->tex[1]);
    if (bound != 0) {
        NrrCoreDxTexStatus(bound);
    }
}

void nrrcore::NrrCoreDxConst(int stage, uint32_t packed) {
    if (sDx.stageConst[stage] == packed) {
        return;
    }
    DxContextGuard();
    sDx.stageConst[stage] = packed;
    sDx.group[2 + stage] = 0xFFFFu; /* the stage no longer holds a block's words */
    NrrCoreDxPut1(packed, Hdr(TexEnvReg(stage) + 3u, 0xF)); /* TEXENVi_COLOR */
    sDx.touched |= G_TEXENV;
}

void nrrcore::NrrCoreDxDraw2D(NrrCtx* c, const float* verts, uint32_t nVerts) {
    DxDrawGuard();
    DxTouch(c);
    DxLazyDraw();
    if (sDx.attr != NRR_PROG_2D) {
        NrrCoreDxAttr(NRR_PROG_2D);
    }
    if (c->scene2d) {
        if (sDx.proj != 2) {
            NrrCoreDxProj3d(c);
        }
    } else if (sDx.proj != 1) {
        NrrCoreDxProj(static_cast<const C3D_Mtx*>(gdx3ds_nrr_bk_proj2d()), 1);
    }
    NrrCoreDxBuf(verts, NRR_PROG_2D, GDX3DS_VBOPACK_STRIDE * sizeof(float));
    if (GDX_UNLIKELY(gdx3ds_stereo_replay_frame)) {
        NrrCoreDxStereo(c, verts);
    }
    NrrCoreDxPut(sArraysWords, kArraysWords)[6] = nVerts;
    NrrCoreDxNoteDraw();
}

void nrrcore::NrrCoreDxDrawXf(NrrCtx* c, const void* verts, uint32_t stride, const uint16_t* idx, uint32_t nIdx,
                              const float* unif28x4, uint32_t rows) {
    DxDrawGuard();
    DxTouch(c);
    DxLazyDraw();
    const int prog = stride == 16 ? NRR_PROG_XF : NRR_PROG_XF_F;
    if (sDx.attr != prog) {
        NrrCoreDxAttr(prog);
    }
    if (sDx.proj != 2) {
        NrrCoreDxProj3d(c);
    }
    sDrawSt.xfDraws++;
    NrrCoreDxXfUniforms(unif28x4, rows);
    if (GDX_UNLIKELY(gdx3ds_stereo_replay_frame)) {
        NrrCoreDxStereo(c, nullptr);
    }
    NrrCoreDxBuf(verts, prog, stride);
    const uint32_t pa = osConvertVirtToPhys(idx);
    if (pa < kBufferBasePaddr) {
        return; /* C3D_DrawElements draws nothing either */
    }
    uint32_t* p = NrrCoreDxPut(sElementsWords, kElementsWords);
    p[4] = (pa - kBufferBasePaddr) | (static_cast<uint32_t>(C3D_UNSIGNED_SHORT) << 31);
    p[6] = nIdx;
    NrrCoreDxNoteDraw();
}

uint32_t nrrcore::NrrDxEndFlags(uint32_t* unifGroups) {
    static const uint32_t kFlag[12] = {
        uint32_t(C3DiF_AttrInfo),                 /* G_ATTR */
        uint32_t(C3DiF_BufInfo),                  /* G_BUF */
        uint32_t(C3DiF_Effect),                   /* G_EFFECT */
        uint32_t(C3DiF_Viewport | C3DiF_Scissor), /* G_VP */
        uint32_t(C3DiF_TexEnvAll),                /* G_TEXENV */
        uint32_t(C3DiF_TexEnvBuf),                /* G_ENVBUF */
        uint32_t(C3DiF_TexAll),                   /* G_TEX */
        0, 0, 0,                                  /* G_PROJ G_XF G_BOOL: uniforms */
        uint32_t(C3DiF_Scissor),                  /* G_SCISSOR */
        0,                                        /* G_STK: a uniform */
    };
    const uint32_t t = sDx.touched;
    uint32_t f = 0;
    for (int g = 0; g < 12; g++) {
        f |= (t & (1u << g)) ? kFlag[g] : 0u;
    }
    *unifGroups = t & (G_PROJ | G_XF | G_BOOL | G_STK);
    return f | sDx.deferred; /* n60draw carry: what citro3d still had pending at the begin */
}

void nrrcore::NrrCoreDxEnd(NrrCtx* c) {
    (void)c;
    uint32_t unif = 0;
    const uint32_t f = NrrDxEndFlags(&unif);
    C3Di_GetContext()->flags |= f;
    if (unif != 0) {
        NrrCoreDxEndUniforms(unif);
    }
    DxMarkEnd(f);
}

int nrrcore::NrrDxProjLoc() {
    return gBk.projLoc;
}

int nrrcore::NrrDxXfLoc() {
    return gBk.xfUnifLoc;
}

int nrrcore::NrrDxStkLoc() {
    return gBk.stkLoc;
}

/* n60draw: the [nrrdraw] receipt of one window (gdx3ds_nrr.cpp EmitDirectWindow). */
GDX_COLD void nrrcore::NrrDxDrawWindow(unsigned long frame) {
    if (gDraw == 0) {
        return;
    }
    const DrawStats& d = sDrawSt;
    gdx3ds_nrr_log("[nrrdraw] frame=%lu bits=0x%lx | handoffs=%u adjacent=%u carried=%u ref flags=%u unif=%u | lazy vp=%u "
                   "tex=%u state=%u | guard fast=%u full=%u | xf draws=%u hinted=%u rows=%u",
                   frame, (unsigned long)gDraw, (unsigned)gDxStats.handoffs, (unsigned)d.adjacent, (unsigned)d.carried,
                   (unsigned)d.refFlags, (unsigned)d.refUnif, (unsigned)d.lazyVp, (unsigned)d.lazyTex,
                   (unsigned)d.lazyState, (unsigned)d.guardFast, (unsigned)d.guardFull, (unsigned)d.xfDraws,
                   (unsigned)d.xfHinted, (unsigned)d.xfRows);
    std::memset(&sDrawSt, 0, sizeof(sDrawSt));
}
