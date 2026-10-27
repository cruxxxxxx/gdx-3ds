/* gdx3ds_nrr_mach.cpp -- n60 NRR milestone 2: machines from a game-side snapshot
 * (gdx3ds_nrr_mach.h for the contract, docs/research/native/n60mach-progress.md for the design,
 * the inventory, the gates and the numbers).
 *
 * Three parts, by thread and temperature:
 *   game side (game fiber): the snapshot slots, the readiness query, the lever latch;
 *   recorder (drawing thread, COLD): a legacy machine draw inside an armed window (the stub's
 *     stock commands for a not-yet-recorded racer) hands every bound run here; when every
 *     non-empty run of the mesh was bound, each run becomes a core state block
 *     (NrrStateBuildInto: the shader, depth/decal/blend/cull/alpha test, scissor, and owned copies
 *     of the bound textures), plus its recorded uniform block and combiner inputs, and which of
 *     those inputs take the racer's colour;
 *   draw (drawing thread, HOT, inside the core's hand-off): per snapshot entry, the model matrix
 *     from the snapshot Mtx, MP = MV * P (P = the interpreter's projection at the marker, i.e. this
 *     view's), the light and FLX alpha light in model space, then per run: NrrStateApply,
 *     NrrSetInputs/NrrSetFog with the racer's colours, NrrDrawXf of the resident index range.
 *     Hot symbols (tools/native/nrr_size.sh): every NrrMachHot* function here and in
 *     gfx_citro3d_nrrmach.inc, plus the `mach` lines of tools/native/nrr_hot.list (the backend
 *     setters and the C entry points the loop calls). */
#include "nrr/gdx3ds_nrr_mach.h"
#include "nrr/gdx3ds_nrr.h"
#include "nrr/gdx3ds_nrr_deco.h" /* n60deco: the decorations share the recorder and the run loop */
#include "nrr/gdx3ds_nrr_crumb.h" /* n60hwdiag */

#include <3ds.h>
#include <citro3d.h>
#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_cpucut.h"
#include "gdx3ds_dlb_shim.h"
#include "gdx3ds_hotcold.h"
#include "gdx3ds_interp_seam.h"
#include "gdx3ds_mesh.h"
#include "gdx3ds_vbopack.h"
#include "gdx3ds_vtx_math.h"
#include "gdx_interp.h"

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern "C" void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern "C" int gdx3ds_c5tick_consume(int sec, void* (*stub)(void* gfx, int sec, int view)) __attribute__((weak));
/* racer.c (decomp-3ds-nrr-mach.patch); absent in harness links. */
extern "C" void* Racer_NrrMachStub(void* gfx, int sec, int view) __attribute__((weak));

#define NRR_HOT __attribute__((noinline))

/* [n60ships] time-sliced card A/B (port/3ds/gfx/gdx3ds_ships.h; weak: harness links without it). */
extern "C" int gdx3ds_nrr_ab_mask __attribute__((weak));
extern "C" int gdx3ds_nrr_ab_phase __attribute__((weak));
extern "C" int gdx3ds_rt_mode(void) __attribute__((weak)); /* N60TICK4 */
static inline bool NrrAbOn(int bit) {
    return &gdx3ds_nrr_ab_mask == nullptr || (gdx3ds_nrr_ab_mask & bit) == 0 || gdx3ds_nrr_ab_phase != 0;
}

/* The recorder's core state key of a captured run (defined with the recorder below; n60deco uses it). */
GDX_COLD static bool StageKey(NrrStateKey& k, const GdxNrrBackendRun& b, int cullMode);

namespace {

constexpr int kEntries = 30 * 6; /* body list index = machineIndex * 6 + lod - 1 */
constexpr unsigned kMaxRuns = 64;
constexpr int kMaxTex = 192;
constexpr int kRetireCap = 1024;

/* ---- levers and receipts ---- */
int sOn = 0;
int sLatched = 0;
/* [native] nrr_speck_batch (n60ships; o3ds-cpucuts: DEFAULT 1, was 0): 1 = a LOD-6 speck whose record
 * is the one the previous entry (also a speck) just drew re-uses that draw's state block, fog and
 * uniform block and only re-poses (MP, light, alpha light, colour); exact by construction (the stock
 * path would write the same values). Single-run speck records only. o3ds-cpucuts: such a speck also
 * hands NrrDrawXfRows only the rows a re-pose writes (kXfSpeckRows) instead of all 28, under the
 * nrr_draw bit 2 contract (every other row equals the previous speck's block). Stock order already
 * keeps a predecessor's specks together (a speck's material is the previous LOD 1-5 body's last run),
 * so grouping them further would only reorder draws across materials: not done.
 * Hardware o3ds-3d@0c2668a, 1P 16 machines: 21 of ~28 entries per frame are specks, sb=0 sfast=0. */
int sSpeckBatch = 1;
/* o3ds-cpucuts [debug] mach_split (DEFAULT 0, diagnostic): split the hot racer's time into pose (MP,
 * lights, the mesh's uniform block), apply (state block, combiner inputs, fog) and draw
 * (NrrDrawXfRows: the uniform compare + the draw packet), `[nrrmach] split` per receipt window. */
int sSplit = 0;
struct SplitWin {
    uint64_t pose, apply, draw;
    unsigned ents, runs;
} sSplitWin;

GDX_COLD void Log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 2) {
        n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    buf[n] = '\0';
    if (&gdx3ds_filelog_write != nullptr) {
        gdx3ds_filelog_write(buf, (size_t)n);
    }
    svcOutputDebugString(buf, (size_t)n);
}

int Cfg(const char* sec, const char* key, int fallback) {
    return (&gdx3ds_config_get_int != nullptr) ? gdx3ds_config_get_int(sec, key, fallback) : fallback;
}

struct Window {
    unsigned frames, markers, ents, runs, drops;      /* render side */
    unsigned specks, sfast;                             /* LOD-6 entries drawn; of those on the speck-run path */
    unsigned stubs, nrr, fallback, other;               /* game side */
    unsigned recBegin, recPub, recIncomplete, recRefused, recTex;
    unsigned long cmdFreeMin; /* n60prewarm: smallest free GPU command-list room (words) after a record pass */
    /* nimach (native-review-2026-10-15.md follow-up #2): NrrMachHotDraw dispatches (the marker
     * actually reached gdx3ds_nrr_draw and ran this category's hot callback), whether or not it
     * went on to set markers++ (which only happens when s->n > 0, i.e. an entry was ALREADY
     * native -- 0 by construction on a cold boot with no records yet). hot > 0 with markers == 0
     * is expected and fine on a cold boot; hot == 0 means the marker never reached the dispatch
     * at all, a different and more serious problem. */
    unsigned hot;
} sWin;

GdxInterpSeam* sSeam = nullptr; /* the walking interpreter (gdx3ds_nrr_mach_bind) */
gdxn::Interp* sInterp = nullptr;

/* ---- records ---- */
/* The gpuxf uniform block (Gdx3dsXfDraw from mvp to off) as vec4 rows: what a run sets on top of
 * its mesh (XfFillRun: conv, convw, tg .. seli), the rest is the mesh's (XfFillMesh, XfFillFlx). */
constexpr int kXfVec4 = 28;
constexpr int kXfMvp = 0;
constexpr int kXfConv = 4;  /* conv, convw */
constexpr int kXfAmb = 7;
constexpr int kXfLdir = 9;  /* ldir[0] */
constexpr int kXfLcol = 12; /* lcol[0] */
constexpr int kXfFlx = 26;
constexpr int kXfTg = 17;   /* tg, tcs, uva[2], uvc[2], col, sel, seli */
constexpr int kXfTgN = 9;
constexpr int kXfCol = 23;
static_assert(offsetof(Gdx3dsXfDraw, conv) - offsetof(Gdx3dsXfDraw, mvp) == kXfConv * 16, "xf layout");
static_assert(offsetof(Gdx3dsXfDraw, tg) - offsetof(Gdx3dsXfDraw, mvp) == kXfTg * 16, "xf layout");
static_assert(offsetof(Gdx3dsXfDraw, col) - offsetof(Gdx3dsXfDraw, mvp) == kXfCol * 16, "xf layout");
static_assert(offsetof(Gdx3dsXfDraw, flx) - offsetof(Gdx3dsXfDraw, mvp) == kXfFlx * 16, "xf layout");
static_assert(kXfTg + kXfTgN == kXfFlx, "xf layout");
static_assert(offsetof(Gdx3dsXfDraw, amb) - offsetof(Gdx3dsXfDraw, mvp) == kXfAmb * 16, "xf layout");
static_assert(offsetof(Gdx3dsXfDraw, ldir) - offsetof(Gdx3dsXfDraw, mvp) == kXfLdir * 16, "xf layout");
static_assert(offsetof(Gdx3dsXfDraw, lcol) - offsetof(Gdx3dsXfDraw, mvp) == kXfLcol * 16, "xf layout");

struct RunRec {
    const NrrState* block;
    const uint16_t* idx;
    uint16_t nIdx;
    uint8_t envMask; /* combiner inputs that read the env register (bits 0-6) */
    uint8_t colEnv;  /* the xf constant colour is the env input */
    uint8_t envInh;  /* the run inherits env from the section's gDPSetEnvColor (the racer's colour) */
    uint8_t numInputs;
    uint8_t fogA;    /* nimach: xf.col alpha = the RDP fog colour's alpha at draw time */
    uint8_t pad;
    float in[7][4];       /* the run's combiner inputs (Gdx3dsVboPackAux.inputRgba) */
    float xf[kXfVec4][4]; /* the uniform block it was drawn with */
};

/* A recording run: the record plus the key its block is built from at publish. */
struct StageRun {
    RunRec rec;
    NrrStateKey key;
};

struct MeshRec {
    uint32_t gen;
    uint16_t nRuns;
    uint16_t pad;
    const void* verts;
    RunRec* runs;
    void* blocks; /* nRuns core state blocks (NrrStateBytes each) */
};

/* Record slots: [list index][BODY | HL | SPECK]. SPECK is used by the speck entry alone: the material
 * a speck with no LOD 1-5 predecessor draws with (recorded from a legacy speck that was the first
 * machine draw after the section's setup, as in stock), beside [speck][BODY] = its geometry. */
constexpr int kKinds = 5; /* + nimach: MBODY, MSBODY (the menus' records) */
MeshRec sRec[kEntries][kKinds];
std::atomic<uint32_t> sReady[kEntries][kKinds];
uint32_t sGenSeen = 0;
unsigned long sRecBytes = 0;

/* Owned texture copies: the record must not depend on the texture cache keeping an entry. */
struct OwnTex {
    uint32_t hash, size, param, dim;
    C3D_Tex tex;
};
OwnTex sTex[kMaxTex];
int sTexN = 0;
unsigned long sTexBytes = 0;

/* Deferred frees: a retired record may still be referenced by the GPU queue of the frame in
 * flight; freed at the third frame begin after it was retired. */
struct Retired {
    void* p;
    uint8_t linear;
    uint32_t stamp;
};
Retired sRetired[kRetireCap];
int sRetiredN = 0;
uint32_t sFrameNo = 0;

/* ---- the open recording draw ---- */
unsigned long sFrameCmdFreeMin = 0; /* n60prewarm: this frame's smallest free command-list room after a record pass */
unsigned sFrameRecs = 0;             /* record passes this frame */
unsigned sArm = 0; /* the marker said stock machine draws follow (until the next frame begin) */
unsigned sArmAny = 0; /* machine body / highlight draws since the marker */
int sLastBody = -1;   /* list index of the last body-pass draw since the marker (a speck's predecessor) */
int sRecMenu = 0;     /* nimach: the armed marker was a menu snapshot (records go to MBODY / MSBODY) */

/* ---- nimach (nointerp S5b): the menus' machines ---- */
int sNiMach = 0; /* [native] ni_mach as last latched by gdx3ds_nimach_level (0 when the category
                     is off); NOT decided once at cat-init time -- see gdx3ds_nimach_level. */
struct MenuWindow {
    unsigned frames, markers, ents, runs, drops, stubs, fallback, recPub, recRefused, models;
} sMenuWin;
struct Stage {
    int open, bad, entry, kind, speck;
    unsigned nRuns, got;
    const void* verts;
    StageRun runs[kMaxRuns];
} sStage;

GDX_COLD void RetirePtr(void* p, bool linear) {
    if (p == nullptr) {
        return;
    }
    if (sRetiredN >= kRetireCap) {
        return; /* leak rather than free under the GPU (never reached: generations are rare) */
    }
    sRetired[sRetiredN].p = p;
    sRetired[sRetiredN].linear = linear ? 1 : 0;
    sRetired[sRetiredN].stamp = sFrameNo;
    sRetiredN++;
}

GDX_COLD void DecoRetire();

GDX_COLD void RetireAll() {
    DecoRetire(); /* the decoration records use the same owned textures */
    for (int e = 0; e < kEntries; e++) {
        for (int k = 0; k < kKinds; k++) {
            MeshRec& m = sRec[e][k];
            RetirePtr(m.runs, false);
            RetirePtr(m.blocks, false);
            m.runs = nullptr;
            m.blocks = nullptr;
            m.nRuns = 0;
            m.gen = 0;
        }
    }
    for (int i = 0; i < sTexN; i++) {
        RetirePtr(sTex[i].tex.data, true);
    }
    sTexN = 0;
    sTexBytes = 0;
    sRecBytes = 0;
}

GDX_COLD void DrainRetired() {
    int keep = 0;
    for (int i = 0; i < sRetiredN; i++) {
        if (sFrameNo - sRetired[i].stamp >= 3u) {
            if (sRetired[i].linear) {
                NRR_CRUMB_FIRST(NRR_CRUMB_MACHTEX, 256, "mach linearFree(%p) retired at frame %lu", sRetired[i].p,
                                (unsigned long)sRetired[i].stamp);
                linearFree(sRetired[i].p);
            } else {
                free(sRetired[i].p);
            }
        } else {
            sRetired[keep++] = sRetired[i];
        }
    }
    sRetiredN = keep;
}

uint32_t Fnv(const void* p, size_t n, uint32_t h) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* A content signature of a texture: FNV over 32 evenly spaced 32-byte samples (all of a small
 * texture). Dedupe key only -- a record never reads a texture through it. */
uint32_t TexSignature(const C3D_Tex* t) {
    const uint8_t* d = (const uint8_t*)t->data;
    const size_t n = t->size;
    if (n <= 1024) {
        return Fnv(d, n, 2166136261u);
    }
    uint32_t h = 2166136261u;
    const size_t step = (n - 32) / 31;
    for (int i = 0; i < 32; i++) {
        h = Fnv(d + (size_t)i * step, 32, h);
    }
    return h;
}

/* n60hwdiag: every linearAlloc the recorder makes, with both ends of the copy. */
GDX_COLD void CrumbOwnTexture(const C3D_Tex* src, const void* data) {
    char a[96];
    char b[96];
    gdx3ds_nrr_crumb_buf(a, sizeof(a), "src", src->data);
    gdx3ds_nrr_crumb_buf(b, sizeof(b), "copy", data);
    NRR_CRUMB_FIRST(NRR_CRUMB_MACHTEX, 256, "mach tex #%d linearAlloc(%lu) %ux%u fmt=%d param=%08lx %s %s%s linfree=%uK",
                    sTexN + 1, (unsigned long)src->size, (unsigned)src->width, (unsigned)src->height, (int)src->fmt,
                    (unsigned long)src->param, a, b, data == nullptr ? " FAILED" : "", gdx3ds_nrr_crumb_linfree_kb());
}

/* An owned copy of a bound texture (data + the sampler state it was bound with). */
GDX_COLD C3D_Tex* OwnTexture(const C3D_Tex* src) {
    if (src == nullptr || src->data == nullptr || src->size == 0) {
        return nullptr;
    }
    const uint32_t h = TexSignature(src);
    for (int i = 0; i < sTexN; i++) {
        OwnTex& t = sTex[i];
        if (t.hash == h && t.size == src->size && t.param == src->param && t.dim == src->dim &&
            t.tex.fmt == src->fmt) {
            return &t.tex;
        }
    }
    if (sTexN >= kMaxTex) {
        return nullptr;
    }
    void* data = linearAlloc(src->size);
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbOwnTexture(src, data);
    }
    if (data == nullptr) {
        return nullptr;
    }
    memcpy(data, src->data, src->size);
    gdx3ds_mesh_backend_flush_resident(data, src->size);
    OwnTex& t = sTex[sTexN++];
    t.hash = h;
    t.size = src->size;
    t.param = src->param;
    t.dim = src->dim;
    t.tex = *src;
    t.tex.data = data;
    sTexBytes += src->size;
    sWin.recTex++;
    if (sTexN <= 48) {
        Log("[nrr] mach tex #%d %ux%u fmt=%d size=%lu param=%08lx src=%p", sTexN, (unsigned)src->width,
            (unsigned)src->height, (int)src->fmt, (unsigned long)src->size, (unsigned long)src->param, src->data);
    }
    return &t.tex;
}

GDX_COLD void StageRefuse() {
    sStage.bad = 1;
    sWin.recRefused++;
}

GDX_COLD void CrumbPublish(const MeshRec* m, bool ok) {
    char vb[96];
    char ib[96];
    gdx3ds_nrr_crumb_buf(vb, sizeof(vb), "verts", sStage.verts);
    gdx3ds_nrr_crumb_buf(ib, sizeof(ib), "idx0", sStage.got != 0 ? sStage.runs[0].rec.idx : nullptr);
    NRR_CRUMB_FIRST(NRR_CRUMB_MACHREC, 24, "mach record pass end entry=%d kind=%d runs=%u %s %s runs=%p blocks=%p -> %s "
                    "recBytes=%lu texBytes=%lu", sStage.entry, sStage.kind, sStage.got, vb, ib,
                    m != nullptr ? (void*)m->runs : nullptr, m != nullptr ? m->blocks : nullptr,
                    ok ? "published" : "REFUSED", sRecBytes, sTexBytes);
}

/* nimach: a menu record was published (first 40 logged with what its runs use). */
GDX_COLD void MenuRecLog(const RunRec* runs) {
    static int sLogged = 0;
    sMenuWin.recPub++;
    if (sLogged >= 40) {
        return;
    }
    sLogged++;
    const RunRec& r0 = runs[0];
    Log("[nimach] rec idx=%d kind=%d runs=%u lit=%d tg=%d flx=%d envMask=0x%02x inh=%u", sStage.entry, sStage.kind,
        sStage.got, r0.xf[kXfAmb][3] != 0.0f ? 1 : 0, r0.xf[kXfTg][0] != 0.0f ? 1 : 0,
        r0.xf[kXfFlx][3] != 0.0f ? 1 : 0, (unsigned)r0.envMask, (unsigned)r0.envInh);
}

GDX_COLD void Publish() {
    const size_t bytes = sizeof(RunRec) * sStage.got;
    const size_t blockBytes = NrrStateBytes();
    RunRec* runs = (RunRec*)malloc(bytes);
    uint8_t* blocks = (uint8_t*)malloc(blockBytes * sStage.got);
    bool ok = runs != nullptr && blocks != nullptr;
    for (unsigned r = 0; ok && r < sStage.got; r++) {
        runs[r] = sStage.runs[r].rec;
        runs[r].block = NrrStateBuildInto(&sStage.runs[r].key, blocks + r * blockBytes);
        ok = runs[r].block != nullptr;
    }
    if (!ok) {
        free(runs);
        free(blocks);
        sWin.recRefused++;
        if (GDX_NRR_CRUMBS_ON()) {
            CrumbPublish(nullptr, false);
        }
        return;
    }
    MeshRec& m = sRec[sStage.entry][sStage.kind];
    RetirePtr(m.runs, false); /* a re-record (not expected: ready meshes are never emitted) */
    RetirePtr(m.blocks, false);
    m.runs = runs;
    m.blocks = blocks;
    m.verts = sStage.verts;
    m.nRuns = (uint16_t)sStage.got;
    m.gen = gdx3ds_mesh_generation();
    sRecBytes += bytes + blockBytes * sStage.got;
    sReady[sStage.entry][sStage.kind].store(m.gen, std::memory_order_release);
    sWin.recPub++;
    if (sStage.kind >= GDX_NRR_MACH_MBODY) {
        MenuRecLog(runs);
    }
    static int sSpeckLogged = 0;
    if (sStage.kind == GDX_NRR_MACH_SPECK && sSpeckLogged < 12) {
        sSpeckLogged++;
        const RunRec& r0 = runs[0];
        Log("[nrr] mach speck rec slot=%d runs=%u nIdx=%u lit=%d flx=%d tg=%d colEnv=%u envMask=0x%02x inh=%u ins=%u",
            sStage.entry, sStage.got, (unsigned)r0.nIdx, r0.xf[kXfAmb][3] != 0.0f ? 1 : 0,
            r0.xf[kXfFlx][3] != 0.0f ? 1 : 0, r0.xf[kXfTg][0] != 0.0f ? 1 : 0, (unsigned)r0.colEnv,
            (unsigned)r0.envMask, (unsigned)r0.envInh, (unsigned)r0.numInputs);
    }
    if (GDX_NRR_CRUMBS_ON()) {
        CrumbPublish(&m, true);
    }
}

/* nimach: every 64 frames while a menu marker was drawn (silent otherwise). */
GDX_COLD void MenuReceipt() {
    if (sMenuWin.markers != 0 || sMenuWin.stubs != 0 || sMenuWin.models != 0) {
        Log("[nimach] frames=%u stubs=%u markers=%u ents=%u runs=%u drops=%u fallback=%u | rec pub=%u refused=%u | "
            "models=%u",
            sMenuWin.frames, sMenuWin.stubs, sMenuWin.markers, sMenuWin.ents, sMenuWin.runs, sMenuWin.drops,
            sMenuWin.fallback, sMenuWin.recPub, sMenuWin.recRefused, sMenuWin.models);
    }
    memset(&sMenuWin, 0, sizeof(sMenuWin));
}

GDX_COLD void SplitReceipt() {
    if (!sSplit || sSplitWin.ents == 0) {
        return;
    }
    const double us = 1000000.0 / (double)SYSCLOCK_ARM11;
    const double perF = sWin.frames != 0 ? 1.0 / (double)sWin.frames : 1.0;
    Log("[nrrmach] split frames=%u ents/f=%.1f runs/f=%.1f us/f pose=%.0f apply=%.0f draw=%.0f | us/run apply=%.1f "
        "draw=%.1f",
        sWin.frames, sSplitWin.ents * perF, sSplitWin.runs * perF, (double)sSplitWin.pose * us * perF,
        (double)sSplitWin.apply * us * perF, (double)sSplitWin.draw * us * perF,
        sSplitWin.runs != 0 ? (double)sSplitWin.apply * us / sSplitWin.runs : 0.0,
        sSplitWin.runs != 0 ? (double)sSplitWin.draw * us / sSplitWin.runs : 0.0);
    memset(&sSplitWin, 0, sizeof(sSplitWin));
}

GDX_COLD void Receipt() {
    SplitReceipt();
    Log("[nrr] mach frames=%u markers=%u hot=%u ents=%u runs=%u drops=%u | stub=%u nrr=%u fallback=%u "
        "other=%u | rec begin=%u pub=%u incomplete=%u refused=%u tex=%u | meshes %luB tex %d/%luB | speck=%u "
        "sfast=%u sb=%d cmdfree_min=%lu",
        sWin.frames, sWin.markers, sWin.hot, sWin.ents, sWin.runs, sWin.drops, sWin.stubs, sWin.nrr, sWin.fallback,
        sWin.other, sWin.recBegin, sWin.recPub, sWin.recIncomplete, sWin.recRefused, sWin.recTex, sRecBytes, sTexN,
        sTexBytes, sWin.specks, sWin.sfast, gdx3ds_cpucut_on(sSpeckBatch, GDX3DS_CPUCUT_BIT_SPECK) != 0 && NrrAbOn(2) ? 1 : 0, sWin.cmdFreeMin);
    memset(&sWin, 0, sizeof(sWin));
}

/* ---- the hot draw ---- */

/* SpMatrix's s15.16 conversion (gdx_rsp.cpp), host words. */
inline void MtxToFloat(const int32_t* addr, float m[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            const int32_t ip = addr[i * 2 + j / 2];
            const uint32_t fp = (uint32_t)addr[8 + i * 2 + j / 2];
            m[i][j] = (int32_t)((ip & 0xffff0000) | (fp >> 16)) / 65536.0f;
            m[i][j + 1] = (int32_t)((ip << 16) | (fp & 0xffff)) / 65536.0f;
        }
    }
}

/* Interp::MatrixMul. */
inline void MatMul(float res[4][4], const float a[4][4], const float b[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            res[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
}

/* What every run of one entry shares: the model-space light, alpha light and MP. */
struct Pose {
    float mvp[4][4]; /* xmvp rows (transposed MP) */
    float ldir[3];
    float flx[3];
    float rgb[3];
};

/* lit / flx: the record's uniform block uses the model-space light / the FLX alpha light
 * (NrrMachHotMeshXf copies them only then); unused, they are not computed ([n60ships]). */
NRR_HOT void NrrMachHotPose(const GdxNrrMachSnap* f, const GdxNrrMachEnt& e, const float (*P)[4],
                         const float alphaRsp[3], bool lit, bool flx, Pose& p) {
    float mv[4][4];
    float mp[4][4];
    MtxToFloat(e.mtx, mv);
    MatMul(mp, mv, P);
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            p.mvp[i][k] = mp[k][i];
        }
    }
    for (int k = 0; k < 3; k++) {
        p.rgb[k] = gdx3ds_byte_to_unit(e.rgb[k]);
    }
    if (lit) {
        float c[3];
        gdx3ds_vtx_light_coeffs(f->ldir, mv, c);
        for (int k = 0; k < 3; k++) {
            p.ldir[k] = c[k] / 127.0f;
        }
    }
    if (flx) {
        gdx3ds_vtx_alpha_light(alphaRsp, mv, p.flx);
    }
}

/* Shared, per view: the light colours, the fog colour and the RSP alpha light. */
struct ViewIn {
    float amb[3];
    float lcol[3];
    float fog[3];
    float alphaRsp[3];
    float fogA; /* nimach (menus): the RDP fog colour's alpha (a fogA run's xf.col alpha) */
};

/* The mesh part of the uniform block for one racer: the record's (fog, unlit, the unused lights,
 * look, the FLX switch, off) with this racer's MP, light and alpha light. */
NRR_HOT void NrrMachHotMeshXf(const MeshRec& m, const Pose& p, const ViewIn& v, float xf[kXfVec4][4], bool same) {
    if (!same) { /* same: xf still holds this record's block from the previous speck (nrr_speck_batch) */
        memcpy(xf, m.runs[0].xf, sizeof(float) * kXfVec4 * 4);
    }
    memcpy(xf[kXfMvp], p.mvp, sizeof(p.mvp));
    if (xf[kXfAmb][3] != 0.0f) {
        memcpy(xf[kXfAmb], v.amb, sizeof(v.amb));
        memcpy(xf[kXfLdir], p.ldir, sizeof(p.ldir));
        memcpy(xf[kXfLcol], v.lcol, sizeof(v.lcol));
    }
    if (xf[kXfFlx][3] != 0.0f) {
        memcpy(xf[kXfFlx], p.flx, sizeof(p.flx));
    }
}

/* One recorded run for one racer: its block, its combiner inputs with the racer's colour on the
 * env inputs (a body: those it inherits from the section's gDPSetEnvColor; a speck: all of them,
 * its gDPSetEnvColor follows the predecessor's last run), the fog-stage colour (the view's fog;
 * the highlight pass: its blend colour), and its run uniforms over the racer's mesh uniforms. */
/* n60draw: the rows NrrMachHotRun writes over the mesh's block (conv, convw, tg..seli incl. col):
 * the only rows that differ between two runs of one mesh (NrrDrawXfRows, [native] nrr_draw bit 2). */
constexpr uint32_t kXfRunRows = (3u << kXfConv) | (((1u << kXfTgN) - 1u) << kXfTg);
constexpr uint32_t kXfAllRows = (1u << kXfVec4) - 1u;
/* o3ds-cpucuts: the rows a nrr_speck_batch re-pose writes over the previous speck's block (MP, the
 * model-space light, the racer colour, the FLX alpha light); conv / tg..seli come from the same run
 * record and amb / lcol from the same view, so they equal the previous block. */
constexpr uint32_t kXfSpeckRows = (0xFu << kXfMvp) | (1u << kXfLdir) | (1u << kXfCol) | (1u << kXfFlx);
static_assert((kXfSpeckRows & ~kXfAllRows) == 0u, "xf layout");

NRR_HOT void NrrMachHotRun(NrrCtx* c, const RunRec& rr, const void* verts, const Pose& p, const ViewIn& v, int kind,
                           float xf[kXfVec4][4], uint32_t rows) {
    memcpy(xf[kXfConv], rr.xf[kXfConv], sizeof(float) * 2 * 4);
    memcpy(xf[kXfTg], rr.xf[kXfTg], sizeof(float) * kXfTgN * 4);
    const float(*in)[4] = rr.in;
    float envIn[7][4];
    const bool envOn = kind == GDX_NRR_MACH_SPECK || (kind == GDX_NRR_MACH_BODY && rr.envInh);
    if (envOn && rr.envMask != 0) {
        memcpy(envIn, rr.in, sizeof(envIn));
        for (unsigned j = 0; j < rr.numInputs; j++) {
            if (rr.envMask & (1u << j)) {
                memcpy(envIn[j], p.rgb, sizeof(p.rgb));
            }
        }
        in = envIn;
    }
    if (envOn && rr.colEnv) {
        memcpy(xf[kXfCol], p.rgb, sizeof(p.rgb));
    }
    if (GDX_UNLIKELY(sSplit)) {
        const uint64_t t0 = svcGetSystemTick();
        NrrStateApply(c, rr.block);
        NrrSetInputs(c, rr.block, in, rr.numInputs);
        NrrSetFog(c, rr.block, kind == GDX_NRR_MACH_HL ? p.rgb : v.fog);
        const uint64_t t1 = svcGetSystemTick();
        NrrDrawXfRows(c, verts, 16, rr.idx, rr.nIdx, &xf[0][0], rows);
        sSplitWin.apply += t1 - t0;
        sSplitWin.draw += svcGetSystemTick() - t1;
        sSplitWin.runs++;
        return;
    }
    NrrStateApply(c, rr.block);
    NrrSetInputs(c, rr.block, in, rr.numInputs);
    NrrSetFog(c, rr.block, kind == GDX_NRR_MACH_HL ? p.rgb : v.fog);
    NrrDrawXfRows(c, verts, 16, rr.idx, rr.nIdx, &xf[0][0], rows);
}

inline const MeshRec* NrrMachHotLiveRec(int idx, int kind, uint32_t gen) {
    const MeshRec& m = sRec[idx][kind];
    return (m.gen == gen && m.runs != nullptr) ? &m : nullptr;
}

/* Returns the record drawn (nullptr when dropped). `prev`: the record the previous entry drew, when
 * that entry was a speck (the nrr_speck_batch path), else nullptr. */
NRR_HOT const MeshRec* NrrMachHotRacer(NrrCtx* c, const GdxNrrMachSnap* f, const GdxNrrMachEnt& e,
                                       const float (*P)[4], const ViewIn& v, uint32_t gen, const MeshRec* prev,
                                       float xf[kXfVec4][4]) {
    const bool speck = e.kind == GDX_NRR_MACH_SPECK;
    const int slot = !speck ? e.idx : (e.idx != GDX_NRR_MACH_NOPRED ? e.idx : GDX_NRR_MACH_SPECK_IDX);
    const MeshRec* m = NrrMachHotLiveRec(slot, speck ? GDX_NRR_MACH_SPECK : e.kind, gen);
    if (GDX_UNLIKELY(m == nullptr)) {
        sWin.drops++; /* the generation moved between the snapshot and the draw (a rebake) */
        return nullptr;
    }
    sWin.runs += m->nRuns;
    sWin.ents++;
    /* [n60ships] nrr_speck_batch: the previous entry drew this same single-run record, so the core
     * holds its block and fog and xf holds its whole uniform block; only the pose and the colour
     * differ. Same values as the full path, written by less code. */
    const bool same = speck && m == prev && m->nRuns == 1;
    sWin.specks += speck ? 1u : 0u;
    sWin.sfast += same ? 1u : 0u;
    Pose p;
    const float(*tmpl)[4] = m->runs[0].xf;
    const uint64_t t0 = GDX_UNLIKELY(sSplit) ? svcGetSystemTick() : 0;
    NrrMachHotPose(f, e, P, v.alphaRsp, tmpl[kXfAmb][3] != 0.0f, tmpl[kXfFlx][3] != 0.0f, p);
    NrrMachHotMeshXf(*m, p, v, xf, same);
    if (GDX_UNLIKELY(sSplit)) {
        sSplitWin.pose += svcGetSystemTick() - t0;
        sSplitWin.ents++;
    }
    const uint32_t firstRows = same ? kXfSpeckRows : kXfAllRows;
    for (unsigned r = 0; r < m->nRuns; r++) {
        NrrMachHotRun(c, m->runs[r], m->verts, p, v, e.kind, xf, r == 0 ? firstRows : kXfRunRows);
    }
    return m;
}

NRR_HOT void NrrMachHotFrame(NrrCtx* c, const GdxNrrMachSnap* f, const float (*P)[4]) {
    const uint32_t gen = gdx3ds_mesh_generation();
    ViewIn v;
    for (int k = 0; k < 3; k++) {
        v.alphaRsp[k] = f->alpha[k] / 256.0f;
        v.amb[k] = (float)f->amb[k];
        v.lcol[k] = (float)f->lcol[k];
        v.fog[k] = f->fog[k] / 255.0f;
    }
    gdx3ds_vtx_normalize(v.alphaRsp);
    float xf[kXfVec4][4];
    const MeshRec* prev = nullptr;
    const bool speckBatch = gdx3ds_cpucut_on(sSpeckBatch, GDX3DS_CPUCUT_BIT_SPECK) != 0 && NrrAbOn(2);
    for (unsigned i = 0; i < f->n; i++) {
        const GdxNrrMachEnt& e = f->e[i];
        const MeshRec* m = NrrMachHotRacer(c, f, e, P, v, gen, prev, xf);
        prev = (speckBatch && e.kind == GDX_NRR_MACH_SPECK) ? m : nullptr;
    }
}

/* ================================================ nimach: the menus' machines ======== */

/* Pose for a menu entry: as NrrMachHotPose with the entry's light set (menus: no FLX alpha light). */
NRR_HOT void NrrMachMenuHotPose(const GdxNrrMachMenuSnap* s, const GdxNrrMachMenuEnt& e, const float (*P)[4],
                                bool lit, Pose& p) {
    float mv[4][4];
    float mp[4][4];
    MtxToFloat(e.mtx, mv);
    MatMul(mp, mv, P);
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            p.mvp[i][k] = mp[k][i];
        }
    }
    for (int k = 0; k < 3; k++) {
        p.rgb[k] = gdx3ds_byte_to_unit(e.rgb[k]);
    }
    if (lit) {
        float c[3];
        gdx3ds_vtx_light_coeffs(s->ldir[e.light & 1], mv, c);
        for (int k = 0; k < 3; k++) {
            p.ldir[k] = c[k] / 127.0f;
        }
    }
}

/* One recorded run of a menu body: as NrrMachHotRun's body pass, with the fog stage colour and the
 * fog-factor alpha of the RDP registers at the marker (what the stock draw reads: the menus never
 * set them). */
NRR_HOT void NrrMachMenuHotRun(NrrCtx* c, const RunRec& rr, const void* verts, const Pose& p, const ViewIn& v,
                               float xf[kXfVec4][4], uint32_t rows) {
    memcpy(xf[kXfConv], rr.xf[kXfConv], sizeof(float) * 2 * 4);
    memcpy(xf[kXfTg], rr.xf[kXfTg], sizeof(float) * kXfTgN * 4);
    const float(*in)[4] = rr.in;
    float envIn[7][4];
    const float* envRgb = p.rgb;
    if (rr.envInh && rr.envMask != 0) {
        memcpy(envIn, rr.in, sizeof(envIn));
        for (unsigned j = 0; j < rr.numInputs; j++) {
            if (rr.envMask & (1u << j)) {
                memcpy(envIn[j], envRgb, sizeof(float) * 3);
            }
        }
        in = envIn;
    }
    if (rr.envInh && rr.colEnv) {
        memcpy(xf[kXfCol], envRgb, sizeof(float) * 3);
    }
    if (rr.fogA) {
        xf[kXfCol][3] = v.fogA;
    }
    NrrStateApply(c, rr.block);
    NrrSetInputs(c, rr.block, in, rr.numInputs);
    NrrSetFog(c, rr.block, v.fog);
    NrrDrawXfRows(c, verts, 16, rr.idx, rr.nIdx, &xf[0][0], rows);
}

/* One menu entry: its viewport through the interpreter (exactly the stock gSPViewport + the first
 * draw's ApplyViewportScissor: the backend folds it, [vpfix]) -- applied through the same
 * NrrEnd/ApplyViewportScissor/NrrBegin cycle niview's NrrFxColdMultiView (gdx_nrr_fx.cpp) uses for
 * an in-hand-off viewport change, one idiom instead of two (native-review-2026-10-15.md Sec.2).
 * NrrBegin's own reset already invalidates the projection (c->projSet = 0), so the explicit
 * NrrProjInvalidate below only matters when the viewport did not change (dirty check false). */
NRR_HOT void NrrMachMenuHotEnt(NrrCtx* c, const GdxNrrMachMenuSnap* s, const GdxNrrMachMenuEnt& e, const MeshRec* m,
                               const float (*P)[4], const ViewIn* v, float xf[kXfVec4][4]) {
    sInterp->NrrMachViewport(e.vp);
    if (sInterp->ViewportScissorDirty()) {
        NrrEnd(c);
        sInterp->ApplyViewportScissor();
        NrrBegin(c);
    }
    NrrProjInvalidate(c);
    const ViewIn& vi = v[e.light & 1];
    Pose p;
    NrrMachMenuHotPose(s, e, P, m->runs[0].xf[kXfAmb][3] != 0.0f, p);
    NrrMachHotMeshXf(*m, p, vi, xf, false);
    for (unsigned r = 0; r < m->nRuns; r++) {
        NrrMachMenuHotRun(c, m->runs[r], m->verts, p, vi, xf, r == 0 ? kXfAllRows : kXfRunRows);
    }
    gdx3ds_mesh_apply_tail(*sSeam, e.idx, e.rgb); /* the registers its stock draw leaves */
    sMenuWin.runs += m->nRuns;
    sMenuWin.ents++;
}

NRR_HOT void NrrMachMenuHotFrame(NrrCtx* c, const GdxNrrMachMenuSnap* s, const float (*P)[4]) {
    const uint32_t gen = gdx3ds_mesh_generation();
    ViewIn v[2];
    memset(v, 0, sizeof(v));
    /* Fog colour: the RDP register the stock draws read (the menus never set it; a body's blender
     * may read it, as the race's specular glint does). */
    int16_t fogMul = 0;
    int16_t fogOff = 0;
    sInterp->NrrFxHotFog(&fogMul, &fogOff, v[0].fog);
    memcpy(v[1].fog, v[0].fog, sizeof(v[1].fog));
    uint8_t fogA = 0;
    sInterp->NrrMachFogAlpha(&fogA);
    v[0].fogA = v[1].fogA = gdx3ds_byte_to_unit(fogA);
    for (int l = 0; l < 2; l++) {
        for (int k = 0; k < 3; k++) {
            v[l].amb[k] = (float)s->amb[l][k];
            v[l].lcol[k] = (float)s->lcol[l][k];
        }
    }
    float xf[kXfVec4][4];
    bool touched = false;
    for (unsigned i = 0; i < s->n; i++) {
        const GdxNrrMachMenuEnt& e = s->e[i];
        const MeshRec* m = NrrMachHotLiveRec(e.idx, e.kind, gen);
        if (GDX_UNLIKELY(m == nullptr)) {
            sMenuWin.drops++;
            continue;
        }
        if (!touched) {
            /* The hand-off's first state write saves citro3d's viewport / scissor (NrrEnd restores
             * them) -- before the first entry's viewport changes them. */
            NrrStateApply(c, m->runs[0].block);
            touched = true;
        }
        NrrMachMenuHotEnt(c, s, e, m, P, v, xf);
    }
    if (touched) {
        sInterp->NrrMachViewportDirty(); /* the walk's next draw re-applies its viewport / scissor */
    }
}

/* ================================================ n60deco: the decorations ======== */

/* The decoration bodies are dress entries of the mesh registry (gdx3ds_mesh.cpp, after the
 * machine entries). A decoration's record = its one or two mesh draws, each a MeshRec of the
 * machine pattern; the hot path reuses NrrMachHotMeshXf / NrrMachHotRun (no env, no lights: a
 * lit or FLX run is refused at record time). */
constexpr int kDressEntries = 48;     /* gdx3ds_mesh.cpp kDressEntries */
constexpr unsigned kDecoMaxRuns = 8;  /* a decoration body binds 1-4 runs */

struct DecoRec {
    MeshRec m[GDX_NRRDECO_MAX_DRAWS];
    uint8_t n;
};

struct DecoDrawStage {
    unsigned got;
    const void* verts;
    StageRun runs[kDecoMaxRuns];
};

/* The open record window (REC BEGIN .. END) and the mesh draw inside it. */
struct DecoStage {
    int open;
    int drawOpen;
    int bad;
    int key;
    unsigned want;
    unsigned got;
    DecoDrawStage d[GDX_NRRDECO_MAX_DRAWS];
} sDecoStage;

struct DecoWin {
    unsigned frames, snaps, ents, meshes, runs, drops;   /* render side */
    unsigned cold, never, full;                          /* game side */
    unsigned windows, recDraws, recPub, recIncomplete, recRefused, retires;
} sDecoWin;

int sDecoOn = 0;
DecoRec sDecoRec[GDX_NRRDECO_KEYS];
uint32_t sDecoReady[GDX_NRRDECO_KEYS]; /* sDecoEpoch at publish. Render-thread writes; under the
                                          N60TICK4 tick-ahead pipe the game reads it concurrently
                                          (a one-frame-lagged view, see n60tick4-progress.md) */
/* N60TICK4: a course change seen by the game while a render thread exists. DecoRetire frees and
 * memsets records the render thread may be drawing, so the game only posts the request and the
 * render thread retires at its next deco frame begin; deco_ready reads 0 until then. */
int sDecoCourseReq = 0;
uint32_t sDecoEpoch = 1;
uint32_t sDecoGen = 0;         /* mesh generation the records belong to */
uint32_t sDecoDressEpoch = 0;  /* gdx3ds_mesh_dress_epoch the records belong to */
uint32_t sDecoSig = 0;
int sDecoRecs = 0;
unsigned long sDecoBytes = 0;

GDX_COLD void DecoRetire() {
    for (int k = 0; k < GDX_NRRDECO_KEYS; k++) {
        DecoRec& d = sDecoRec[k];
        for (int j = 0; j < GDX_NRRDECO_MAX_DRAWS; j++) {
            RetirePtr(d.m[j].runs, false);
            RetirePtr(d.m[j].blocks, false);
        }
        memset(&d, 0, sizeof(d));
    }
    sDecoEpoch++;
    sDecoRecs = 0;
    sDecoBytes = 0;
    sDecoWin.retires++;
}

/* The records are of the current mesh generation and dress bakes (else retired, cold). A new
 * generation retires the owned textures too (RetireAll: the machines' own frame begin would). */
GDX_COLD void DecoCheckEpochs() {
    const uint32_t gen = gdx3ds_mesh_generation();
    const uint32_t dress = gdx3ds_mesh_dress_epoch();
    if (gen != sDecoGen) {
        sDecoGen = gen;
        sDecoDressEpoch = dress;
        RetireAll();
    } else if (dress != sDecoDressEpoch) {
        sDecoDressEpoch = dress;
        DecoRetire();
    }
}

GDX_COLD void DecoRefuse() {
    sDecoStage.bad = 1;
    sDecoWin.recRefused++;
}

GDX_COLD bool DecoBuildMesh(const DecoDrawStage& d, MeshRec& m) {
    const size_t blockBytes = NrrStateBytes();
    RunRec* runs = (RunRec*)malloc(sizeof(RunRec) * d.got);
    uint8_t* blocks = (uint8_t*)malloc(blockBytes * d.got);
    bool ok = runs != nullptr && blocks != nullptr;
    for (unsigned r = 0; ok && r < d.got; r++) {
        runs[r] = d.runs[r].rec;
        runs[r].block = NrrStateBuildInto(&d.runs[r].key, blocks + r * blockBytes);
        ok = runs[r].block != nullptr;
    }
    if (!ok) {
        free(runs);
        free(blocks);
        return false;
    }
    m.runs = runs;
    m.blocks = blocks;
    m.verts = d.verts;
    m.nRuns = (uint16_t)d.got;
    m.gen = sDecoGen;
    sDecoBytes += (sizeof(RunRec) + blockBytes) * d.got;
    return true;
}

GDX_COLD void DecoPublish() {
    DecoRec rec;
    memset(&rec, 0, sizeof(rec));
    for (unsigned j = 0; j < sDecoStage.got; j++) {
        if (!DecoBuildMesh(sDecoStage.d[j], rec.m[j])) {
            for (unsigned i = 0; i < j; i++) {
                free(rec.m[i].runs);
                free(rec.m[i].blocks);
            }
            sDecoWin.recRefused++;
            return;
        }
    }
    rec.n = (uint8_t)sDecoStage.got;
    DecoRec& d = sDecoRec[sDecoStage.key];
    for (int j = 0; j < GDX_NRRDECO_MAX_DRAWS; j++) {
        RetirePtr(d.m[j].runs, false); /* a re-record (not expected: a ready key is never emitted) */
        RetirePtr(d.m[j].blocks, false);
    }
    d = rec;
    sDecoReady[sDecoStage.key] = sDecoEpoch;
    sDecoRecs++;
    sDecoWin.recPub++;
    if (sDecoWin.recPub <= 64 && sDecoRecs <= 64) {
        const RunRec& r0 = rec.m[0].runs[0];
        Log("[nrr] deco rec key=%d (type %d owner %d) draws=%u runs=%u/%u fog=%d tex=%d", sDecoStage.key,
            sDecoStage.key / GDX_NRRDECO_OWNERS, sDecoStage.key % GDX_NRRDECO_OWNERS - 1,
            sDecoStage.got, (unsigned)rec.m[0].nRuns, rec.n > 1 ? (unsigned)rec.m[1].nRuns : 0u,
            r0.block != nullptr ? 1 : 0, sTexN);
    }
}

/* The record window of one stock decoration (REC BEGIN), unless its key is recorded already. */
GDX_COLD void DecoRecOpen(uint32_t w1) {
    const int key = (int)(w1 & 0xFFFFu);
    const unsigned want = (w1 >> 16) & 0xFFu;
    DecoCheckEpochs();
    sDecoStage.open = 0;
    gdx3ds_nrr_deco_arm = 0;
    if (key < 0 || key >= GDX_NRRDECO_KEYS || want == 0 || want > GDX_NRRDECO_MAX_DRAWS ||
        sDecoReady[key] == sDecoEpoch) {
        return;
    }
    sDecoStage.open = 1;
    sDecoStage.drawOpen = 0;
    sDecoStage.bad = 0;
    sDecoStage.key = key;
    sDecoStage.want = want;
    sDecoStage.got = 0;
    gdx3ds_nrr_deco_arm = 1;
    sDecoWin.windows++;
}

/* REC END: publish when every expected mesh draw was recorded whole. */
GDX_COLD void DecoRecClose() {
    gdx3ds_nrr_deco_arm = 0;
    if (!sDecoStage.open) {
        return;
    }
    sDecoStage.open = 0;
    if (!sDecoStage.bad && sDecoStage.got == sDecoStage.want) {
        DecoPublish();
    } else if (!sDecoStage.bad) {
        sDecoWin.recIncomplete++;
    }
}

GDX_COLD int DecoRecBegin(int entryIdx, const void* verts) {
    sDecoStage.drawOpen = 0;
    if (!sDecoStage.open || sDecoStage.bad || entryIdx >= kEntries + kDressEntries || verts == nullptr) {
        return 0;
    }
    if (sDecoStage.got >= sDecoStage.want) {
        DecoRefuse(); /* more mesh draws than the type has */
        return 0;
    }
    DecoDrawStage& d = sDecoStage.d[sDecoStage.got];
    d.got = 0;
    d.verts = verts;
    sDecoStage.drawOpen = 1;
    sDecoWin.recDraws++;
    return 1;
}

GDX_COLD void DecoRecRun(const Gdx3dsXfDraw* xf, const Gdx3dsVboPackAux* aux, const uint16_t* idx, uint32_t nIdx) {
    if (!sDecoStage.drawOpen || sDecoStage.bad) {
        return;
    }
    DecoDrawStage& d = sDecoStage.d[sDecoStage.got];
    if (d.got >= kDecoMaxRuns || nIdx > 0xFFFFu || xf->verts != d.verts) {
        DecoRefuse();
        return;
    }
    GdxNrrBackendRun b;
    memset(&b, 0, sizeof(b));
    if (!gdx3ds_nrr_backend_capture(&b) || !b.fogOk) {
        DecoRefuse();
        return;
    }
    StageRun& sr = d.runs[d.got];
    memset(&sr, 0, sizeof(sr));
    RunRec& rr = sr.rec;
    memcpy(rr.xf, xf->mvp, sizeof(rr.xf));
    if (rr.xf[kXfAmb][3] != 0.0f || rr.xf[kXfFlx][3] != 0.0f) {
        DecoRefuse(); /* lit / FLX: the pose would need the view's lights */
        return;
    }
    if (!StageKey(sr.key, b, xf->cullMode)) {
        DecoRefuse();
        return;
    }
    rr.idx = idx;
    rr.nIdx = (uint16_t)nIdx;
    rr.numInputs = aux->numInputs < 7 ? aux->numInputs : 7;
    memcpy(rr.in, aux->inputRgba, sizeof(rr.in));
    d.got++;
}

GDX_COLD void DecoRecEnd(int need) {
    sDecoStage.drawOpen = 0;
    if (sDecoStage.bad) {
        return;
    }
    DecoDrawStage& d = sDecoStage.d[sDecoStage.got];
    if (need <= 0 || (unsigned)need != d.got) {
        sDecoStage.bad = 1;
        sDecoWin.recIncomplete++;
        return;
    }
    sDecoStage.got++;
}

GDX_COLD void DecoReceipt() {
    Log("[nrr] deco frames=%u snaps=%u ents=%u meshes=%u runs=%u drops=%u | game cold=%u never=%u full=%u | rec "
        "windows=%u draws=%u pub=%u incomplete=%u refused=%u retires=%u | recs=%d %luB tex %d/%luB",
        sDecoWin.frames, sDecoWin.snaps, sDecoWin.ents, sDecoWin.meshes, sDecoWin.runs, sDecoWin.drops,
        sDecoWin.cold, sDecoWin.never, sDecoWin.full, sDecoWin.windows, sDecoWin.recDraws, sDecoWin.recPub,
        sDecoWin.recIncomplete, sDecoWin.recRefused, sDecoWin.retires, sDecoRecs, sDecoBytes, sTexN, sTexBytes);
    memset(&sDecoWin, 0, sizeof(sDecoWin));
}

/* ---- the decorations' hot draw ---- */

/* SpMatrix's conversion and Interp::MatrixMul, out of line: the decorations call each up to three
 * times per entry, and one copy of each is cheaper to keep in the L1-I than three. */
NRR_HOT void NrrDecoHotMtx(const int32_t* addr, float m[4][4]) {
    MtxToFloat(addr, m);
}

NRR_HOT void NrrDecoHotMul(float res[4][4], const float (*a)[4], const float (*b)[4]) {
    MatMul(res, a, b);
}

/* One recorded mesh of a decoration under model-view `mv`: the machine run loop, kind BODY with
 * no inherited env (a decoration's inputs are its record's). */
NRR_HOT void NrrDecoHotMesh(NrrCtx* c, const MeshRec& m, const float (*mv)[4], const float (*P)[4], const ViewIn& v,
                            float xf[kXfVec4][4]) {
    float mp[4][4];
    NrrDecoHotMul(mp, mv, P);
    Pose p;
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            p.mvp[i][k] = mp[k][i];
        }
    }
    NrrMachHotMeshXf(m, p, v, xf, false);
    for (unsigned r = 0; r < m.nRuns; r++) {
        NrrMachHotRun(c, m.runs[r], m.verts, p, v, GDX_NRR_MACH_BODY, xf, r == 0 ? kXfAllRows : kXfRunRows);
    }
    sDecoWin.runs += m.nRuns;
    sDecoWin.meshes++;
}

/* One decoration: its body under its matrix; the second body under rotation x matrix (the stock
 * G_MTX_MUL: SpMatrix's MatrixMul(mv, rot, mv)). */
NRR_HOT void NrrDecoHotEnt(NrrCtx* c, const GdxNrrDecoEnt& e, const float (*P)[4], const ViewIn& v,
                           float xf[kXfVec4][4]) {
    if (GDX_UNLIKELY(sDecoReady[e.key] != sDecoEpoch)) {
        sDecoWin.drops++; /* retired between the snapshot and the draw */
        return;
    }
    const DecoRec& d = sDecoRec[e.key];
    float mv[4][4];
    NrrDecoHotMtx(e.base, mv);
    NrrDecoHotMesh(c, d.m[0], mv, P, v, xf);
    if (d.n > 1 && e.mul != nullptr) {
        float rot[4][4];
        float mv2[4][4];
        NrrDecoHotMtx(e.mul, rot);
        NrrDecoHotMul(mv2, rot, mv);
        NrrDecoHotMesh(c, d.m[1], mv2, P, v, xf);
    }
    sDecoWin.ents++;
}

/* The core's draw callback for NRR_CAT_DECO (inside its hand-off; the walk's viewport and the
 * game's clip square kept, as the machines). Fog colour: the RDP register the stock pass draws
 * with (it never sets it). */
NRR_HOT int NrrDecoHotDraw(NrrCtx* c, int view, const NrrFrame* f) {
    const GdxNrrDecoSnap* s = static_cast<const GdxNrrDecoSnap*>(NrrSnapGet(f, NRR_CAT_DECO, view));
    if (s == nullptr) {
        return 1;
    }
    if (sInterp == nullptr) {
        return 2;
    }
    sDecoWin.snaps++;
    sInterp->ApplyViewportScissor();
    c->keepVp = 1;
    ViewIn v;
    memset(&v, 0, sizeof(v));
    int16_t fogMul = 0;
    int16_t fogOff = 0;
    sInterp->NrrFxHotFog(&fogMul, &fogOff, v.fog);
    const float(*P)[4] = sInterp->RspState().P_matrix;
    float xf[kXfVec4][4];
    for (unsigned i = 0; i < s->n; i++) {
        NrrDecoHotEnt(c, s->e[i], P, v, xf);
    }
    return 0;
}

} // namespace

/* ================================================================= game side ======== */

extern "C" int gdx3ds_nrr_mach_ready(int idx, int kind) {
    if (idx < 0 || idx >= kEntries || kind < 0 || kind >= kKinds) {
        return 0;
    }
    const uint32_t g = sReady[idx][kind].load(std::memory_order_acquire);
    return g != 0 && g == gdx3ds_mesh_generation();
}

extern "C" void* gdx3ds_nrr_mach_publish(void* gfx, int view, const GdxNrrMachSnap* s, int fallback, int other) {
    sWin.stubs++;
    sWin.nrr += s->n;
    sWin.fallback += (unsigned)fallback;
    sWin.other += (unsigned)other;
    if (s->n == 0 && !s->rec) {
        return gfx;
    }
    NrrFrame* f = gdx3ds_nrr_frame_build();
    const uint32_t bytes = (uint32_t)(offsetof(GdxNrrMachSnap, e) + sizeof(GdxNrrMachEnt) * s->n);
    GdxNrrMachSnap* dst = f != nullptr ? (GdxNrrMachSnap*)NrrSnapAlloc(f, bytes) : nullptr;
    if (dst == nullptr) {
        return gfx;
    }
    memcpy(dst, s, bytes);
    NrrSnapSet(f, NRR_CAT_MACH, view, dst);
    return gdx3ds_nrr_write_marker(gfx, NRR_CAT_MACH, view, f);
}

/* Menu-only call (MachineSelect_NimachMachines / MachineSettings_NimachBodies, once per frame
 * each: not the race hot path), so re-deriving here instead of trusting a value cached once at
 * gdx3ds_nrr_cat_init_mach time is cheap and, unlike that boot-time snapshot, always current.
 * Fixes an init-order report (2026-10-15, nointerp combined build @6f8eff4f): `[nimach] init`
 * logged REFUSED in the same run where `[nrr] init` showed mach=1 -- mach's cat init can run (and
 * nimach's old one-shot latch fired) before every NRR category -- and whatever else shares its
 * boot path in the combined tree / under nimenu's force_mode -- has settled `sOn`. Re-checking on
 * every real use means a transient early miss self-heals the moment the menu actually draws. */
extern "C" int gdx3ds_nimach_level(void) {
    const int niMach = Cfg("native", "ni_mach", 0);
    const int level = sOn ? (niMach & (GDX_NIMACH_SELECT | GDX_NIMACH_SETTINGS)) : 0;
    if (level != sNiMach) {
        Log("[nimach] relatch ni_mach=%d (was %d; asked %d, mach on=%d)%s", level, sNiMach, niMach, sOn,
            (niMach != 0 && !sOn) ? " REFUSED: needs the nrr_mach category on" : "");
        sNiMach = level;
    }
    return sNiMach;
}

extern "C" void* gdx3ds_nimach_model_marker(void* gfx) {
    uint32_t* w = static_cast<uint32_t*>(gfx); /* the port's Gfx: w0, pad, w1 (64-bit) */
    w[0] = (GDX_NIMACH_MODEL_P << 16) | 1u;    /* G_NOOP = 0 */
    w[1] = 0;
    w[2] = 0;
    w[3] = 0;
    sMenuWin.models++;
    return w + 4;
}

extern "C" void* gdx3ds_nrr_mach_menu_publish(void* gfx, const GdxNrrMachMenuSnap* s, int fallback) {
    sMenuWin.stubs++;
    sMenuWin.fallback += (unsigned)fallback;
    if (sNiMach == 0 || s->n > GDX_NRR_MACH_MENU_MAX) {
        return nullptr;
    }
    NrrFrame* f = gdx3ds_nrr_frame_build();
    const uint32_t bytes = (uint32_t)(offsetof(GdxNrrMachMenuSnap, e) + sizeof(GdxNrrMachMenuEnt) * s->n);
    GdxNrrMachMenuSnap* dst = f != nullptr ? (GdxNrrMachMenuSnap*)NrrSnapAlloc(f, bytes) : nullptr;
    if (dst == nullptr) {
        return nullptr;
    }
    memcpy(dst, s, bytes);
    dst->tag = GDX_NRR_MACH_MENU_TAG;
    NrrSnapSet(f, NRR_CAT_MACH, 0, dst);
    return gdx3ds_nrr_write_marker(gfx, NRR_CAT_MACH, 0, f);
}

/* n60stereo: under [stereo] enabled the NRR draws through the stereo frame replay
 * (gdx3ds_stereo_frame.cpp); only its killswitch keeps the categories mono-only. */
extern "C" int gdx3ds_stereo_nrr_ok(void) __attribute__((weak));
GDX_COLD static bool NrrStereoOk() {
    return &gdx3ds_stereo_nrr_ok == nullptr || gdx3ds_stereo_nrr_ok() != 0;
}

GDX_COLD static const char* NrrMachRefusal() {
    if (Cfg("native", "gpuxf", 0) != 1) {
        return "needs [native] gpuxf=1";
    }
    if (Cfg("native", "machines_retained", 0) != 1) {
        return "needs [native] machines_retained=1";
    }
    if (Cfg("native", "replay", 0) != 0) {
        return "refused under [native] replay (C6 re-submits the legacy GX queue)";
    }
    if (!NrrStereoOk()) {
        return "refused under [stereo] enabled with [native] stereo_replay=0 (the frame replay draws its right eye)";
    }
    if (&Racer_NrrMachStub == nullptr || &gdx3ds_c5tick_consume == nullptr) {
        return "no stub linked (decomp-3ds-nrr-mach.patch)";
    }
    return nullptr;
}

NRR_HOT int NrrMachHotDraw(NrrCtx* c, int view, const NrrFrame* f);

/* [n60ships] the time-sliced card A/B ([debug] nrr_ab bit 0): in an off block the stub declines and
 * the BODY section builds the stock way (game state identical: the stub makes the section's writes,
 * and so does the section). Otherwise the decomp's stub. */
static void* NrrMachAbStub(void* gfx, int sec, int view) {
    if (!NrrAbOn(1)) {
        return nullptr;
    }
    return Racer_NrrMachStub(gfx, sec, view);
}

/* The core calls this once at boot when [native] nrr=1 nrr_mach=1 (gdx3ds_nrr.h, a category). */
extern "C" void gdx3ds_nrr_cat_init_mach(void) {
    if (sLatched) {
        return;
    }
    sLatched = 1;
    const char* why = NrrMachRefusal();
    if (why == nullptr && !gdx3ds_c5tick_consume(GDX_DLB_SEC_BODY, NrrMachAbStub)) {
        why = "the BODY consume was refused";
    }
    sOn = why == nullptr ? 1 : 0;
    if (sOn) {
        gdx3ds_nrr_set_draw(NRR_CAT_MACH, NrrMachHotDraw);
    }
    sSpeckBatch = sOn ? gdx3ds_cpucut_clamp(Cfg("native", "nrr_speck_batch", 1), 1) : 0;
    gdx3ds_cpucut_lever("speck", &sSpeckBatch, GDX3DS_CPUCUT_BIT_SPECK);
    sSplit = Cfg("debug", "mach_split", 0) != 0 ? 1 : 0;
    /* ni_mach itself is NOT decided here: gdx3ds_nimach_level() derives it lazily, on every real
     * (menu-only) use, from sOn as it stands at that moment -- not as it stood during this boot
     * call, which other categories / combined-build boot paths may not have settled yet. This
     * receipt is just what was asked; gdx3ds_nimach_level logs the live value the first time it
     * is read, and again on every change. */
    Log("[nimach] init asked=%d (bits 1 machine select, 2 machine settings / GP next machine; see "
        "the first `[nimach] relatch` receipt for the live value)",
        Cfg("native", "ni_mach", 0));
    /* 2 = A/B: on in the even [cpucut] blocks of bit 3 (o3ds-cpucuts), and further time-sliced by
     * [debug] nrr_ab bit 1 + nrr_ab_period when that is set */
    Log("[nrr] mach speck_batch=%d (asked %d; 1 = consecutive specks of one single-run record re-use its state "
        "and uniforms, exact) mach_split=%d",
        sSpeckBatch, Cfg("native", "nrr_speck_batch", 1), sSplit);
    Log("[nrr] mach init on=%d%s%s (body consumed on rendered ticks; snapshot <= %u B/view; hot: NrrMachHot* + "
        "the core's state/draw calls)",
        sOn, why != nullptr ? " REFUSED: " : "", why != nullptr ? why : "", (unsigned)sizeof(GdxNrrMachSnap));
}

void gdx3ds_nrr_mach_bind(GdxInterpSeam* seam, gdxn::Interp* interp) {
    sSeam = seam;
    sInterp = interp;
}

/* nimach (GP ending scene scope, native-review-2026-10-15.md follow-up): whether the BODY
 * section's native path is armed at all ([native] nrr_mach + its other preconditions, settled
 * once by gdx3ds_nrr_cat_init_mach above). gdx3ds_c5tick.c's gdx_dl_scene reads this so a
 * non-race, non-records 3D scene (the GP ending's own Racer_Draw(gfx, 0) call, outside
 * Race_Draw's RHEAD..TRANS bracket) can still open the BODY consume's scope; without it
 * gdx_dl_skip(BODY, ...) never calls the registered stub there (sRaceScope and sSceneScope
 * both false), whatever Racer_NrrMachStub's own gGameMode check says. */
extern "C" int gdx3ds_nrr_mach_armed(void) {
    return sOn;
}

/* ================================================================= recorder ========= */

extern "C" int gdx3ds_nrr_mach_recording(int entryIdx) {
    if (entryIdx >= kEntries) {
        return sDecoStage.open && !sDecoStage.bad && entryIdx < kEntries + kDressEntries; /* n60deco */
    }
    return sArm != 0 && entryIdx >= 0;
}

/* A speck draw records the material it was drawn with into the slot of its predecessor in the
 * stock-ordered legacy stream: [P][SPECK] after the body of list P, [speck idx][SPECK] when it is
 * the window's first machine draw (no body, no highlight before it: the section's setup state).
 * -1 = no slot (only a highlight pass before it). */
GDX_COLD static int SpeckSlot() {
    if (sLastBody >= 0) {
        return sLastBody;
    }
    return sArmAny == 0 ? GDX_NRR_MACH_SPECK_IDX : -1;
}

extern "C" int gdx3ds_nrr_mach_rec_begin(int entryIdx, unsigned nRuns, const void* verts) {
    sStage.open = 0;
    sDecoStage.drawOpen = 0;
    if (entryIdx >= kEntries) {
        return DecoRecBegin(entryIdx, verts); /* n60deco: a decoration body */
    }
    if (sArm == 0 || entryIdx < 0 || entryIdx >= kEntries || verts == nullptr || nRuns > kMaxRuns) {
        return 0;
    }
    const bool speck = entryIdx == GDX_NRR_MACH_SPECK_IDX;
    int slot = entryIdx;
    if (speck) {
        slot = SpeckSlot();
        if (slot < 0 || gdx3ds_nrr_mach_ready(slot, GDX_NRR_MACH_SPECK)) {
            return 0;
        }
    } else {
        sArmAny++;
    }
    sStage.open = 1;
    sStage.bad = 0;
    sStage.entry = slot;
    sStage.kind = speck ? GDX_NRR_MACH_SPECK : -1; /* a body's pass is read from its first run */
    sStage.speck = speck ? 1 : 0;
    sStage.nRuns = nRuns;
    sStage.got = 0;
    sStage.verts = verts;
    NRR_CRUMB_FIRST(NRR_CRUMB_MACHREC, 24, "mach record pass begin entry=%d slot=%d speck=%d runs=%u verts=%p", entryIdx,
                    slot, speck ? 1 : 0, nRuns, verts);
    return 1;
}

/* The core state key of a captured run (cold). The textures are the recorder's own copies. */
GDX_COLD static bool StageKey(NrrStateKey& k, const GdxNrrBackendRun& b, int cullMode) {
    memset(&k, 0, sizeof(k));
    k.shaderId0 = b.id0;
    k.shaderId1 = b.id1;
    for (int t = 0; t < 2; t++) {
        k.tex[t] = NRR_TEX_NONE;
        if (b.tex[t] == nullptr) {
            continue;
        }
        k.texObj[t] = OwnTexture(static_cast<const C3D_Tex*>(b.tex[t]));
        if (k.texObj[t] == nullptr) {
            return false;
        }
        k.texLinear[t] = b.texLinear[t];
        k.texCms[t] = b.texCms[t];
        k.texCmt[t] = b.texCmt[t];
    }
    k.program = NRR_PROG_XF;
    k.depthTest = b.depthTest;
    k.depthWrite = b.depthMask;
    k.decal = b.decal;
    k.blend = b.useAlpha ? NRR_BLEND_ALPHA : NRR_BLEND_OFF;
    k.cull = cullMode == GDX3DS_CULL_BACK ? NRR_CULL_BACK : (cullMode == GDX3DS_CULL_FRONT ? NRR_CULL_FRONT : NRR_CULL_NONE);
    k.alphaTest = NRR_AT_OFF; /* the shader's own edge / threshold flags decide (BuildEffect) */
    k.alphaRef = b.alphaRef;
    /* no scissor in the key: the draw keeps the walk's (keepVp), i.e. the view being drawn */
    return true;
}

/* A body pass draws with the racer's env (alpha 255, no blend-colour fog); the highlight pass
 * with the blend colour. Anything else in an armed window (a ghost's env alpha 160) is neither,
 * and is not recorded. */
GDX_COLD static int PassOf(uint8_t envA, int blendColor) {
    if (blendColor != 0) {
        return GDX_NRR_MACH_HL;
    }
    return envA == 255 ? GDX_NRR_MACH_BODY : -1;
}

extern "C" void gdx3ds_nrr_mach_rec_run(const Gdx3dsXfDraw* xf, const Gdx3dsVboPackAux* aux, const uint16_t* idx,
                                        uint32_t nIdx, uint8_t envMask, uint8_t colEnv, uint8_t envA,
                                        int blendColor) {
    if (sDecoStage.drawOpen) {
        DecoRecRun(xf, aux, idx, nIdx); /* n60deco */
        return;
    }
    if (!sStage.open || sStage.bad) {
        return;
    }
    int kind = PassOf(envA, blendColor);
    if (sRecMenu != 0 && kind >= 0) {
        /* nimach: select BODY -> MBODY, settings BODY -> MSBODY; an outline pass is never recorded */
        if (kind == GDX_NRR_MACH_HL) {
            kind = -1;
        } else {
            kind = sRecMenu == GDX_NRR_MACH_CTX_SETTINGS ? GDX_NRR_MACH_MSBODY : GDX_NRR_MACH_MBODY;
        }
        if (kind < 0 || xf->flx[3] != 0.0f) {
            StageRefuse(); /* the menu snapshot carries no FLX alpha light */
            sMenuWin.recRefused++;
            return;
        }
    }
    if (!sStage.speck && sStage.kind < 0) {
        /* the first run of a body / highlight draw: note the predecessor, then record or only watch */
        if (kind == GDX_NRR_MACH_BODY) {
            sLastBody = sStage.entry;
        }
        if (kind < 0 || gdx3ds_nrr_mach_ready(sStage.entry, kind)) {
            sStage.open = 0;
            return;
        }
        sStage.kind = kind;
        sWin.recBegin++;
    } else if (sStage.speck && sStage.got == 0) {
        sWin.recBegin++;
    }
    const int want = sStage.speck ? GDX_NRR_MACH_BODY : sStage.kind;
    if (kind != want || sStage.got >= kMaxRuns || nIdx > 0xFFFFu || xf->verts != sStage.verts) {
        StageRefuse();
        return;
    }
    GdxNrrBackendRun b;
    memset(&b, 0, sizeof(b));
    if (!gdx3ds_nrr_backend_capture(&b) || !b.fogOk) {
        StageRefuse();
        return;
    }
    StageRun& sr = sStage.runs[sStage.got];
    memset(&sr, 0, sizeof(sr));
    if (!StageKey(sr.key, b, xf->cullMode)) {
        StageRefuse();
        return;
    }
    RunRec& rr = sr.rec;
    rr.idx = idx;
    rr.nIdx = (uint16_t)nIdx;
    rr.envMask = (uint8_t)(envMask & 0x7Fu);
    rr.envInh = (uint8_t)(envMask >> 7);
    rr.colEnv = (uint8_t)(colEnv & 1u);
    rr.fogA = (uint8_t)((colEnv >> 1) & 1u);
    rr.numInputs = aux->numInputs < 7 ? aux->numInputs : 7;
    memcpy(rr.in, aux->inputRgba, sizeof(rr.in));
    memcpy(rr.xf, xf->mvp, sizeof(rr.xf));
    sStage.got++;
}

/* `need`: the non-empty runs of the mesh; the record is complete when every one was bound. */
extern "C" void gdx3ds_nrr_mach_rec_end(int need) {
    if (sDecoStage.drawOpen) {
        DecoRecEnd(need); /* n60deco */
        return;
    }
    if (!sStage.open) {
        return;
    }
    sStage.open = 0;
    {
        const unsigned long freeWords = (unsigned long)(gpuCmdBufSize - gpuCmdBufOffset);
        if (sWin.cmdFreeMin == 0 || freeWords < sWin.cmdFreeMin) {
            sWin.cmdFreeMin = freeWords;
        }
        if (sFrameCmdFreeMin == 0 || freeWords < sFrameCmdFreeMin) {
            sFrameCmdFreeMin = freeWords;
        }
        sFrameRecs++;
    }
    if (sStage.bad) {
        return;
    }
    if ((unsigned)need != sStage.got || need <= 0) {
        sWin.recIncomplete++;
        return;
    }
    Publish();
}

/* ================================================================= render side ====== */

GDX_COLD static void DecoFrameBegin() {
    gdx3ds_nrr_deco_arm = 0;
    sDecoStage.open = 0;
    if (__atomic_load_n(&sDecoCourseReq, __ATOMIC_ACQUIRE)) {
        DecoRetire(); /* N60TICK4: the game's course-change request, honoured on the render thread */
        __atomic_store_n(&sDecoCourseReq, 0, __ATOMIC_RELEASE);
    }
    DecoCheckEpochs();
    if (sRetiredN != 0 && !sOn) {
        DrainRetired();
    }
    if (++sDecoWin.frames >= 64) {
        DecoReceipt();
    }
}

extern "C" void gdx3ds_nrr_mach_frame_begin(void) {
    if (sFrameRecs >= 16) {
        /* n60prewarm: a frame with many record passes (a first-sight burst or the opaque-frame prewarm) --
         * how close the frame's GPU command list came to full (svcBreak in C3Dgdx_Reserve). */
        Log("[nrr] mach recframe passes=%u cmdlist free_min=%lu words (now %lu/%lu)", sFrameRecs, sFrameCmdFreeMin,
            (unsigned long)gpuCmdBufOffset, (unsigned long)gpuCmdBufSize);
    }
    sFrameRecs = 0;
    sFrameCmdFreeMin = 0;
    sFrameNo++;
    sArm = 0;
    sRecMenu = 0;
    if (sNiMach != 0 && ++sMenuWin.frames >= 64) {
        MenuReceipt();
    }
    sArmAny = 0;
    sLastBody = -1;
    if (sDecoOn) {
        DecoFrameBegin();
    }
    if (!sOn) {
        return;
    }
    const uint32_t gen = gdx3ds_mesh_generation();
    if (GDX_UNLIKELY(gen != sGenSeen)) {
        sGenSeen = gen;
        if (GDX_NRR_CRUMBS_ON()) {
            gdx3ds_nrr_crumb("mach retire all: mesh gen=%lu tex=%d texBytes=%lu (linearFree in 3 frames)", (unsigned long)gen,
                             sTexN, sTexBytes);
        }
        RetireAll();
    }
    if (sRetiredN != 0) {
        DrainRetired();
    }
    if (++sWin.frames >= 64) {
        Receipt();
    }
}

/* nimach: a menu snapshot's marker (machine select / settings / GP next machine). */
NRR_HOT static int NrrMachMenuHotDraw(NrrCtx* c, const GdxNrrMachMenuSnap* s) {
    sRecMenu = s->rec ? s->ctx : 0;
    sArm = s->rec;
    sArmAny = 0;
    sLastBody = -1;
    if (s->n == 0) {
        return 0;
    }
    if (sInterp == nullptr || sSeam == nullptr) {
        return 2;
    }
    c->keepVp = 1;
    NrrMachMenuHotFrame(c, s, sInterp->RspState().P_matrix);
    sMenuWin.markers++;
    return 0;
}

/* The core's draw callback for NRR_CAT_MACH, inside its hand-off (NrrBegin has drawn the
 * interpreter's open batch; NrrEnd restores every citro3d group the blocks changed, so the
 * backend's mirrors and the interpreter's render-state cache stay true by construction). The
 * draws keep the walk's viewport: a split-screen view's machines stay in their view. */
NRR_HOT int NrrMachHotDraw(NrrCtx* c, int view, const NrrFrame* f) {
    sWin.hot++;
    const GdxNrrMachSnap* s = static_cast<const GdxNrrMachSnap*>(NrrSnapGet(f, NRR_CAT_MACH, view));
    if (s == nullptr) {
        return 1;
    }
    if (GDX_UNLIKELY(s->pad0 == GDX_NRR_MACH_MENU_TAG)) {
        return NrrMachMenuHotDraw(c, reinterpret_cast<const GdxNrrMachMenuSnap*>(s));
    }
    sRecMenu = 0;
    sArm = s->rec;
    sArmAny = 0;
    sLastBody = -1;
    if (s->n == 0) {
        return 0;
    }
    if (sInterp == nullptr) {
        return 2;
    }
    sInterp->ApplyViewportScissor(); /* this view's rect, as the stock body's first draw would */
    c->keepVp = 1;
    NrrMachHotFrame(c, s, sInterp->RspState().P_matrix);
    sWin.markers++;
    return 0;
}

/* ================================================================= n60deco API ====== */

int gdx3ds_nrr_deco_arm = 0;

extern "C" int gdx3ds_nrr_deco_on(void) {
    return sDecoOn;
}

extern "C" int gdx3ds_nrr_deco_ready(int key) {
    if (!sDecoOn || key < 0 || key >= GDX_NRRDECO_KEYS || __atomic_load_n(&sDecoCourseReq, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    return sDecoReady[key] == sDecoEpoch && sDecoGen == gdx3ds_mesh_generation() &&
           sDecoDressEpoch == gdx3ds_mesh_dress_epoch();
}

extern "C" void gdx3ds_nrr_deco_course(uint32_t sig) {
    if (sig == sDecoSig) {
        return;
    }
    sDecoSig = sig;
    if (&gdx3ds_rt_mode != nullptr && gdx3ds_rt_mode() != 0) {
        __atomic_store_n(&sDecoCourseReq, 1, __ATOMIC_RELEASE); /* N60TICK4: render thread retires */
        return;
    }
    DecoRetire();
}

extern "C" void* gdx3ds_nrr_deco_publish(void* gfx, const GdxNrrDecoSnap* s, int cold, int never, int fullStock,
                                          int view) {
    sDecoWin.cold += (unsigned)cold;
    sDecoWin.never += (unsigned)never;
    sDecoWin.full += (unsigned)fullStock;
    if (s->n == 0) {
        return gfx;
    }
    NrrFrame* f = gdx3ds_nrr_frame_build();
    const uint32_t bytes = (uint32_t)(offsetof(GdxNrrDecoSnap, e) + sizeof(GdxNrrDecoEnt) * s->n);
    GdxNrrDecoSnap* dst = f != nullptr ? (GdxNrrDecoSnap*)NrrSnapAlloc(f, bytes) : nullptr;
    if (dst == nullptr) {
        return gfx; /* the arena is full: the core counts ovf= (these decorations miss one frame) */
    }
    memcpy(dst, s, bytes);
    NrrSnapSet(f, NRR_CAT_DECO, view, dst); /* niview: 0 in a 1P race, the view of a multi-view one */
    return gdx3ds_nrr_write_marker(gfx, NRR_CAT_DECO, view, f);
}

extern "C" void gdx3ds_nrr_deco_rec_marker(uint32_t w0, uint32_t w1) {
    if (!sDecoOn) {
        return;
    }
    const uint32_t op = (w0 >> 8) & 0xFFu;
    if (op == GDX_NRRDECO_OP_BEGIN) {
        DecoRecOpen(w1);
    } else if (op == GDX_NRRDECO_OP_END) {
        DecoRecClose();
    }
}

GDX_COLD static const char* NrrDecoRefusal() {
    if (gdx3ds_nrr_level(NRR_CAT_DRESS) == 0) {
        return "needs [native] nrr_dress=1 (the dress stub plans the decorations)";
    }
    if (Cfg("native", "gpuxf", 0) != 1) {
        return "needs [native] gpuxf=1";
    }
    if (Cfg("native", "machines_retained", 0) != 1) {
        return "needs [native] machines_retained=1 (resident decoration meshes)";
    }
    if (Cfg("native", "replay", 0) != 0) {
        return "refused under [native] replay";
    }
    if (!NrrStereoOk()) {
        return "refused under [stereo] enabled with [native] stereo_replay=0 (the frame replay draws its right eye)";
    }
    return nullptr;
}

/* The core calls this once at boot when [native] nrr=1 nrr_deco=1 (after the dress category). */
extern "C" void gdx3ds_nrr_cat_init_deco(void) {
    const char* why = NrrDecoRefusal();
    sDecoOn = why == nullptr ? 1 : 0;
    if (sDecoOn) {
        gdx3ds_nrr_set_draw(NRR_CAT_DECO, NrrDecoHotDraw);
    }
    Log("[nrr] deco init on=%d%s%s (decorations recorded on first sight through the mesh path, drawn from "
        "records keyed (type, TMEM owner); snapshot <= %u B; hot: NrrDecoHot* + the machine run loop)",
        sDecoOn, why != nullptr ? " REFUSED: " : "", why != nullptr ? why : "", (unsigned)sizeof(GdxNrrDecoSnap));
}
