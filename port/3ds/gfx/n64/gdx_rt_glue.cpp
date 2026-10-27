/* port/3ds/gfx/n64/gdx_rt_glue.cpp -- render-thread integration of the owned interpreter.
 *
 * gdx_gfx_job_run (n64_gfx_bridge.cpp) calls gdx_ninterp_job_run for every prepared job; this
 * returns 0 (LUS path) unless `[native] interp = 1`. The latch is read once at first use and
 * the receipt line names the path so a log always shows which interpreter drew.
 *
 * Killswitches (plan §3.15, all under [native]):
 *   interp        0/1   owned path on (default 0 until M4 parity)
 *   interp_verify 0/1   M0: bridge-vs-owned opcode histogram per task, [nverify] on mismatch
 *                       M1+: both paths on one job, per-batch hash compare
 *   cpuclip       0/1   Sutherland-Hodgman fallback (M2)
 *
 * This is the only TU in n64/ that includes LUS/citro3d headers. */
#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h> /* atexit: [debug] nicensus final dump */
#include <string.h>

#include <3ds.h>

#include "gdx_dlsrc_bridge.h"
#include "gdx_interp.h"
#include "gdx_perf.h"
#include "gdx_log.h"
#include "gdx_seam_owned.h"
#include "gdx3ds_gpu_prof.h"
#include "gdx3ds_hwprof.h" /* hwprof [hwsite] seam for the interpreter */
#include "gdx3ds_interp_seam.h"
#include "gdx3ds_mesh.h" /* [native] machines_texskip registry queries */
#include "gdx3ds_overlap.h" /* C5 Stage 4 [overlap]: scene-marker kicks */
#include "gdx3ds_skylate.h" /* n60tail: [native] sky_late */
#include "gdx3ds_replay.h"  /* C6 [replay]: the sky category */
#include "gdx3ds_verify.h"
#include "gdx3ds_hotcold.h" /* hotcold: GDX_COLD on the receipts/census below */
#include "nrr/gdx3ds_nrr_sky.h" /* [native] nrr_sky: the p=0x64 learn markers */
#include "nrr/gdx3ds_nrr_fx.h"  /* [native] nrr_shd / nrr_fx: the p=0x66 learn markers */
#include "gfx_citro3d.h"
extern "C" {
#include "../../vendor/citro3d/source/internal.h" /* C3Di_GetContext: [native] nrr_hud */
}
#include "nrr/gdx3ds_nrr.h" /* [native] nrr: the 0x63 marker */
#include "nrr/gdx3ds_nrr_mach.h" /* [native] nrr_mach: binds the walking interpreter */
#include "nrr/gdx3ds_nrr_deco.h" /* [native] nrr_deco: the p=0x67 record markers, the record window */

extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback);
extern "C" int gdx3ds_native_lever(const char* key, int fallback);
extern "C" int gdx3ds_config_loaded(void);
extern "C" void gdx3ds_nrr_hud_pd_set(int value); /* gdx_nrr_hud.cpp (o3ds-cpucuts) */
extern "C" void gdx3ds_filelog_write(const char* s, size_t n);
extern "C" int gdx3ds_trectbatch_enabled(void) __attribute__((weak));
/* The bridge's two memo levers, RESOLVED, for the [native] receipt (n64_gfx_bridge.cpp). */
extern "C" int gdx_bridge_dlfacts_on(void) __attribute__((weak));
extern "C" int gdx_bridge_stubgen_narrow_on(void) __attribute__((weak));
extern "C" int gdx_bridge_memoorder(void) __attribute__((weak));
extern "C" void gdx3ds_atlas_arm(int on) __attribute__((weak));
extern "C" int gdx3ds_tex_page(uint32_t texId) __attribute__((weak));
extern "C" uint32_t gdx3ds_tex_bytes(uint32_t texId) __attribute__((weak)); /* N60TEX tex_keep */
extern "C" void gdx3ds_atlas_reset_all(void) __attribute__((weak)); /* native-all S1 atlas_race_reset */
extern "C" void gdx3ds_vbopack_refresh(Gdx3dsVboPackParams* params) __attribute__((weak));
/* C5 STAGE 0 [c5cat] (port/3ds/gdx3ds_mainprof.h): scene-category timing of the walk. Weak: the
 * DL harness links this library without the main-thread clock. */
extern "C" int gdx3ds_c5cat_enter(int cat) __attribute__((weak));
extern "C" void gdx3ds_c5cat_leave(int prev) __attribute__((weak));
extern "C" void gdx3ds_c5cat_marker(uint32_t w0) __attribute__((weak));
extern "C" void gdx3ds_gpucensus_marker(unsigned category) __attribute__((weak)); /* n60gpu */
/* C5 Stage 3b (port/3ds/gfx/gdx3ds_dress.h): the levers' resolved levels, per scope. */
extern "C" int gdx_dress_level(int scope) __attribute__((weak));
extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact);
/* gfx_citro3d.cpp: the stage-constant input mask of the shader currently bound. */
extern "C" int Gdx_NativeBatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask);
extern "C" void gdx3ds_texcache_note_import(void);
extern "C" void gdx3ds_texcache_note_miss(const void* addr, unsigned fmt, unsigned siz, unsigned tmemWord,
                                          unsigned lineBytes, unsigned sizeBytes, unsigned tileW, unsigned tileH,
                                          unsigned hash);

int NiMenuGameMode(); /* nimenu: below, beside the other game-mode reader */

namespace {

GDX_COLD void RtLog(const char* msg) {
    svcOutputDebugString(msg, strlen(msg));
    gdx3ds_filelog_write(msg, strlen(msg));
}

/* Adapter: gdxn::Backend over the process-lifetime citro3d renderer (unchanged backend). */
class BackendC3D final : public gdxn::Backend {
  public:
    explicit BackendC3D(Fast::GfxRenderingAPIC3D* r) : mR(r) {
    }
    gdxn::ClipParams GetClipParameters() override {
        const Fast::GfxClipParameters p = mR->GetClipParameters();
        return gdxn::ClipParams{ p.z_is_from_0_to_1, p.invertY };
    }
    int GetMaxTextureSize() override {
        return mR->GetMaxTextureSize();
    }
    void UnloadShader(void* prg) override {
        mR->UnloadShader(static_cast<Fast::ShaderProgram*>(prg));
    }
    void* LookupShader(uint64_t id0, uint64_t id1) override {
        return mR->LookupShader(id0, id1);
    }
    void* CreateAndLoadNewShader(uint64_t id0, uint64_t id1) override {
        return mR->CreateAndLoadNewShader(id0, id1);
    }
    void LoadShader(void* prg) override {
        mR->LoadShader(static_cast<Fast::ShaderProgram*>(prg));
    }
    void ShaderGetInfo(void* prg, uint8_t* numInputs, bool used[2]) override {
        mR->ShaderGetInfo(static_cast<Fast::ShaderProgram*>(prg), numInputs, used);
    }
    uint32_t NewTexture() override {
        return mR->NewTexture();
    }
    void SelectTexture(int tile, uint32_t id) override {
        mR->SelectTexture(tile, id);
    }
    void UploadTexture(const uint8_t* rgba32, uint32_t w, uint32_t h) override {
        mR->UploadTexture(rgba32, w, h);
    }
    void SetSamplerParameters(int sampler, bool linear, uint32_t cms, uint32_t cmt) override {
        mR->SetSamplerParameters(sampler, linear, cms, cmt);
    }
    void DeleteTexture(uint32_t id) override {
        mR->DeleteTexture(id);
    }
    int GetTextureFilter() override {
        return static_cast<int>(mR->GetTextureFilter());
    }
    void SetDepthTestAndMask(bool t, bool m) override {
        mR->SetDepthTestAndMask(t, m);
    }
    void SetZmodeDecal(bool d) override {
        mR->SetZmodeDecal(d);
    }
    void SetViewport(int x, int y, int w, int h) override {
        mR->SetViewport(x, y, w, h);
    }
    void SetScissor(int x, int y, int w, int h) override {
        mR->SetScissor(x, y, w, h);
    }
    void SetUseAlpha(bool a) override {
        mR->SetUseAlpha(a);
    }
    void SetCurrentPrimDepth(float d) override {
        mR->SetCurrentPrimDepth(d);
    }
    void SetCurrentAlphaCompareThreshold(float t) override {
        mR->SetCurrentAlphaCompareThreshold(t);
    }
    void DrawTriangles(float buf[], size_t len, size_t tris) override {
        mR->DrawTriangles(buf, len, tris);
    }
    float* VboPackBegin(Gdx3dsVboPackParams* p, uint32_t* capTris) override {
        return mR->VboPackBegin(p, capTris);
    }
    void DrawPackedBatch(size_t tris, const Gdx3dsVboPackAux* aux) override {
        mR->DrawPackedBatch(tris, aux);
    }
    void StartFrame() override {
        mR->StartFrame();
    }
    void UpdateFramebufferParameters(int fb, uint32_t w, uint32_t h, uint32_t msaa, bool invY, bool rt, bool depth,
                                     bool extract) override {
        mR->UpdateFramebufferParameters(fb, w, h, msaa, invY, rt, depth, extract);
    }
    void StartDrawToFramebuffer(int fb, float noise) override {
        mR->StartDrawToFramebuffer(fb, noise);
    }
    void ClearFramebuffer(bool c, bool d) override {
        mR->ClearFramebuffer(c, d);
    }
    void ClearDepthRegion(int x, int y, int w, int h) override {
        mR->ClearDepthRegion(x, y, w, h);
    }
    void FogNoteParams(int mul, int off, int exact) override {
        gdx3ds_fog_note_params(mul, off, exact);
    }
    bool ProfActive() override {
        return gdx3ds_prof_active != 0;
    }
    uint64_t ProfEnter(uint64_t* snap) override {
        return gdx3ds_prof_enter(snap);
    }
    void ProfExit(int sec, uint64_t t0, uint64_t snap) override {
        gdx3ds_prof_exit(sec, t0, snap);
    }
    bool VerifyActive() override {
        return gdx3ds_verify_mode != 0;
    }
    void VerifyNoteTri(const void* tri) override {
        if (gdx3ds_verify_active()) gdx3ds_verify_note_tri(static_cast<const Gdx3dsVerifyTri*>(tri));
    }
    void VerifyNoteCmd(uint32_t w0, uint32_t w1) override {
        if (gdx3ds_verify_active()) gdx3ds_verify_note_cmd(w0, w1);
    }
    void* GetFramebufferTextureId(int fb) override {
        return mR->GetFramebufferTextureId(fb);
    }
    void SelectTextureFb(int fb) override {
        mR->SelectTextureFb(fb);
    }
    void TexCacheNoteImport() override {
        gdx3ds_texcache_note_import();
    }
    void TexCacheNoteMiss(const void* addr, unsigned fmt, unsigned siz, unsigned tmemWord, unsigned lineBytes,
                          unsigned sizeBytes, unsigned tileW, unsigned tileH, unsigned hash) override {
        gdx3ds_texcache_note_miss(addr, fmt, siz, tmemWord, lineBytes, sizeBytes, tileW, tileH, hash);
    }
    bool BatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask) override {
        return Gdx_NativeBatchConstInputs(rgbMask, alphaMask) != 0;
    }
    bool TrectBatchEnabled() override {
        return &gdx3ds_trectbatch_enabled != nullptr && gdx3ds_trectbatch_enabled() != 0;
    }
    void AtlasArm(bool on) override {
        if (&gdx3ds_atlas_arm != nullptr) gdx3ds_atlas_arm(on ? 1 : 0);
    }
    int TexPage(uint32_t id) override {
        return (&gdx3ds_tex_page != nullptr) ? gdx3ds_tex_page(id) : -1;
    }
    uint32_t TexBytes(uint32_t id) override {
        return (&gdx3ds_tex_bytes != nullptr) ? gdx3ds_tex_bytes(id) : 0u;
    }
    size_t LinearFreeBytes() override {
        return linearSpaceFree();
    }
    size_t VboFreeFloats() override {
        return mR->mVboPoolFloats > mR->mVboOffsetFloats ? mR->mVboPoolFloats - mR->mVboOffsetFloats : 0;
    }
    unsigned long AtlasEpoch() override {
        return mR->AtlasResetCount(); /* page evictions + resets: the only events that move a placed view */
    }
    const unsigned long* AtlasEpochPtr() override {
        return mR->AtlasResetCountPtr();
    }
    void AtlasResetAll() override {
        if (&gdx3ds_atlas_reset_all != nullptr) gdx3ds_atlas_reset_all();
    }
    void VboPackRefresh(Gdx3dsVboPackParams* p) override {
        if (&gdx3ds_vbopack_refresh != nullptr) gdx3ds_vbopack_refresh(p);
    }
    bool HsxRun(Gdx3dsHsxProgram* prog) override {
        return mR->HsxRun(prog); /* [native] hudsky_emit: one call per retained HUD run */
    }
    void DrawPackedLean(size_t tris, const Gdx3dsVboPackAux* aux) override {
        mR->DrawPackedLean(tris, aux); /* [native] hudsky_emit: HUD / sky drains */
    }
    bool NrrScissorLive() override {
        return C3Di_GetContext()->scissor[0] != GPU_SCISSOR_DISABLE; /* [native] nrr_hud: the stock scissor quirk */
    }
    float* NrrPackCursor(uint32_t* capTris) override {
        return mR->NrrPackCursor(capTris); /* [native] nrr_hud: a batch begin without the param derivation */
    }

  private:
    Fast::GfxRenderingAPIC3D* mR;
};

int sLatched = 0;   /* 0 = not yet read, 1 = read */
#ifdef EXPANSION_KIT
/* B11o [debug] ek_widget_scissor -- default 1 = the fix (gdx_rect.cpp ScissorPillarSafeX),
 * 0 restores the linear-only scissor box, i.e. restores the clipped drop-down. EK builds
 * only, so the shipped .3dsx never sees this TU change. */
int sEkWidgetScissor = 1;
/* [native] ek_editmesh (gdx_ekpreview.cpp, o3dsek plan S5c): the course editor's preview memo. 0 off
 * (default), 1 serve, 2 verify ([native] ek_editmesh_verify=1: stock draws, the memo compared). */
int sEkEditMesh = 0;
#endif
int sInterpOn = 0;
int sVerifyOn = 0;
int sCpuClip = 0;
int sBlendFlush = 1; /* [native] blendflush: Flush() before a blend-alpha change (LUS parity) */
int sCensus = 0;     /* [native] ncensus: batch-split attribution ([ncensus] receipt) */
int sOwnFold = 0;    /* [native] ownfold: prim/env fold inside the owned batches (B2) */
/* [native] dspcensus: per-opcode SELF-time census of the owned walker (B4). 0 off, 1 opcode
 * self time, 2 + the G_DL sub-phases. A diagnostic: three clock reads per dispatched command,
 * so ms are inflated and only the RANKING is a claim. */
int sDspCensus = 0;
/* [debug] nicensus (gdx_nicensus.cpp; nicensus-progress.md): measurement only, default 0.
 * sNicensusEvery = [debug] nicensus_every, frames between cumulative dumps (default 1800). */
int sNicensus = 0;
int sNicensusEvery = 1800;
int sNicensusWipe = 0; /* menus-s4 [debug] nicensus_wipe: transition frames get their own sub-scope */
/* [debug] ni_strict (native-all-plan.md S7, gdx_nicensus.cpp): dev-only census of every command a
 * stock handler still runs, by screen x context x site x opcode. Default 0. 1 = COUNT (nothing dropped,
 * output identical); 2 = drop is a TODO and counts like 1. It runs on the nicensus engine (sNicensus
 * is raised for it) without nicensus's own [nicensus] receipts unless [debug] nicensus=1 too.
 * sNiStrictEvery = [debug] ni_strict_every, frames between the compact periodic receipts. */
int sNiStrict = 0;
int sNiStrictEvery = 3600;
/* [native] palhash: fold the live TLUT contents into the CI texture-cache key. Both caches key
 * CI entries on the palette's DRAM ADDRESS, which cannot see a TLUT rewritten in place at that
 * same address, so an animated palette serves whatever decode the entry was born with. */
/* [native] timgmemo: the owned walker's cross-frame SETTIMG classification memo (B4's F3
 * target). A stale texture classification is a wrong-pixels bug, which is why the invalidation
 * key was derived in writing first (docs/research/native/loadblock-progress.md S1).
 * PROMOTED to default 1 on native/tmemblock (stubmemo-progress.md S6): its own corpus gate is
 * green (12/12 pairs, 30 972 hashes, 0 divergent -- loadblock-progress.md S4) and the three
 * memo levers were then re-gated TOGETHER at these new defaults. `[native] timgmemo = 0`
 * restores the per-load re-derivation. */
int sPalHash = 1;
int sTexHashCi = 1;  /* [native] texhash_ci: content-key the CI/RGBA32 classes (plan §3.7) */
int sTexInval = 1;   /* [native] texinval: replay the queued cache clear/deletes on the owned path */
/* DROPSITES. sTexStaleFix / sVtxClamp are killswitches over the two fixes (default = fixed);
 * sTexScratchKib / sVtxMax are forcing knobs (default 0 = shipped behaviour). */
int sTexStaleFix = 1;
int sTexKeepKib = 0;      /* N60TEX [debug] tex_keep / tex_keep_kb: 0 = the stock clear */
int sTexKeepFloorKib = 0; /* N60TEX [debug] tex_keep_floor_kb */
int sVtxClamp = 1;
int sTexScratchKib = 0;
int sTexScratchMaxKib = 0;
int sVtxMax = 0;
int sClipPolyMax = 0;
/* [native] rawcopywhole: the bridge's persistent texture copy must span the WHOLE upcoming load,
 * not just the registered extent of the buffer the load starts in. A registered range is
 * bookkeeping, not a memory bound: the game legitimately issues a load that runs past the end of
 * its own buffer (the last strip of a transition reads one row past backgroundBuffer), and on
 * the console DRAM simply continues there -- which is what the owned walk reads. Clamping the
 * copy instead left the LUS interpreter reading past the end of the COPY, i.e. unrelated host
 * heap. Third instance of one class, after the o2r-by-name truncation of 2026-09-17. */
int sRawCopyWhole = 1;
int sTimgMemo = 1;   /* [native] timgmemo: cross-frame SETTIMG classification memo (B4/F3) */
/* [native] tmemskip: widen G_LOADBLOCK/G_LOADTILE's same-content skip past the o2r resource
 * class, using the bridge's own source-stability seam (gdx_dlsrc_bridge.h). 0 off (the shipping
 * immutable-only gate), 1 the classes that are stable with NO compare (native-RGBA16 by
 * generation, registered ROM-backed host ranges), 2 additionally the DMA-tracked class.
 * Default 0 until the corpus gate is green: a stale TMEM mirror is a wrong-pixels bug AND a
 * stale texture-cache identity, so the invalidation argument was written first
 * (docs/research/native/tmemblock-progress.md S2).
 * [native] tmemcls arms the class census alone and changes no behaviour. */
int sTmemSkip = 0;
int sTmemCls = 0;
int sHudCensus = 0; /* [native] hudcensus: per-opcode clock of the replayed HUD runs ([hudop]) */
/* [native] hud_direct (C5 Stage 2b, gdx_hud_direct.cpp): inside a replayed HUD run the loads
 * leave TMEM debt, the texture key's hash is memoised per source, the rectangles are emitted
 * directly. 0 off (default), 1 on, 2 verify (stock path + cross-checks). Needs hud_batch. */
int sHudDirect = 0;
/* [native] hud_retain (c5hud3, gdx_hud_retain.cpp): run-level retention of the direct HUD runs.
 * 0 off (default), 1 on, 2 verify (every would-be hit executes and is compared). Needs
 * hud_batch and hud_direct=1. */
int sHudRetain = 0;
/* [native] sky_retain (0 off, 1 retain, 2 verify, 3 retain + PICA clipping: NOT exact) / skyclock
 * (1 the [skycnt] census, 2 + the [skyop] opcode clock): the sky's
 * p=0x65 markers (gdx_sky_retain.cpp, c5sky-progress.md). Not Tier-2; default 0. */
int sSkyRetain = 0;
int sHudSkyEmit = 0; /* [native] hudsky_emit (hudsky): the compact HUD/sky emitter */
int sSkyClock = 0;
/* [memotear] [debug] tmemfault -- FAULT INJECTION for the tmemskip refusal arm (NEXT.md B54).
 * tmemblock gated tmemskip green over 412,326 corpus skips with stale=0, i.e. the corpus never
 * once drove the gate to say STALE; only the host test does. 0 = off (always, by default).
 * N > 0 = force staleness on every Nth load that WOULD have skipped, alternating the two
 * mechanisms the gate is supposed to catch (the source generation moving, and the class
 * changing under the span). Inert unless tmemskip != 0, so the shipping default cannot reach
 * it. The gate result is the pair (a) tmemStale climbs by the injection count and the skip does
 * not happen, (b) the frame is byte-identical to the tmemskip=0 run -- a refusal means the
 * mirror is rewritten, which is exactly what tmemskip=0 does every time. */
int sTmemFault = 0;
/* o3dsinterp (NEXT.md C4m, o3dsinterp-progress.md). Neither is Tier-2: `all=1` touches neither.
 * [native] o3dsfast (default 1) -- the EXACT rewrites: halfword swap a word at a time, span-store
 *   range fills, a clipper without `%`. Same stores, same floats; 0 restores the historic loops.
 * [native] trimemo (default 0) -- skip ResolveDrawState when a snapshot of everything it reads
 *   is unchanged (Interp::TriMemoHit). Default off until the hardware A/B says it pays. */
int sO3dsFast = 1;
int sTriMemo = 0;
BackendC3D* sBackend = nullptr;
gdxn::Interp* sInterp = nullptr;
GdxInterpSeam* sSeam = nullptr; /* the native replay seam over sInterp (gdx_seam_owned.cpp) */

/* == gfx_noop_handler_f3dex2's marker arms (vendored interpreter.cpp): the track marker
 * (p=0x5A end / 0x5B verify start) and the fx marker (0x5C / 0x5D), handed the command's two
 * words as-is. Under interp=1 the replays run through the owned seam, so the recorded runs
 * draw natively exactly where the stock commands stood -- the course under [native] track and
 * the effect sprites under [native] fx_sink no longer vanish with the owned walker. */
/* C5 STAGE 2 [c5cnt]: per-category WORK counters beside [c5cat]'s per-category time. At every
 * category switch of the walk the interpreter's monotone counters and the [prof] section
 * accumulators are snapshotted and the delta is charged to the category that was open, so the
 * receipt says what a category's milliseconds were spent on (commands walked, rects, TMEM
 * loads, texture imports/decodes, backend draws, and the TRI/IMP/DRW self ms). Live only
 * between a walk that the [c5cat] clock opened and its close (main thread, renderthread=0);
 * every snapshot is therefore inside one task, so the interpreter's per-window ResetStats
 * (after a task) never falls between two of them. One int test otherwise. */
constexpr int kC5CntCats = 13; /* GDX3DS_C5CAT_COUNT */
struct C5CntSnap {
    uint32_t cmds, rects, loads, imports, decodes, batches, draws;
    uint64_t triTicks, impTicks, drwTicks;
};
struct C5CntWin {
    uint64_t cmds, rects, loads, imports, decodes, batches, draws;
    uint64_t triTicks, impTicks, drwTicks;
};
int sC5CntCat = 0; /* category open in the walk; 0 = not live */
C5CntSnap sC5CntAt = {};
C5CntWin sC5CntWin[kC5CntCats] = {};

C5CntSnap C5CntTake() {
    C5CntSnap s;
    const gdxn::Counters& st = sInterp->Stats();
    s.cmds = st.cmds;
    s.rects = st.rects;
    s.loads = st.tmemLoads;
    s.imports = st.texImports;
    s.decodes = st.texMisses;
    s.batches = st.batches;
    s.draws = gdx3ds_prof_sec_calls[GDX3DS_PROF_DRW];
    s.triTicks = gdx3ds_prof_sec_ticks[GDX3DS_PROF_TRI];
    s.impTicks = gdx3ds_prof_sec_ticks[GDX3DS_PROF_IMP];
    s.drwTicks = gdx3ds_prof_sec_ticks[GDX3DS_PROF_DRW];
    return s;
}

void C5CntCharge(int cat, const C5CntSnap& a, const C5CntSnap& b) {
    if (cat <= 0 || cat >= kC5CntCats) {
        return;
    }
    C5CntWin& w = sC5CntWin[cat];
    w.cmds += b.cmds - a.cmds;
    w.rects += b.rects - a.rects;
    w.loads += b.loads - a.loads;
    w.imports += b.imports - a.imports;
    w.decodes += b.decodes - a.decodes;
    w.batches += b.batches - a.batches;
    w.draws += b.draws - a.draws;
    w.triTicks += b.triTicks - a.triTicks;
    w.impTicks += b.impTicks - a.impTicks;
    w.drwTicks += b.drwTicks - a.drwTicks;
}

/* [debug] c5ops=1 (n60hud2, diagnostic): the walker's opcode histogram inside the HUD category
 * ([c5ops] line beside [c5cnt]): which interpreter handlers the HUD phase reaches outside the
 * NRR HUD's own runs (the kept words between the 0x5E markers: setup G_DLs, markers, matrices). */
int sC5Ops = -1;
uint32_t sC5OpsAt[256];
uint64_t sC5OpsWin[256];

GDX_COLD void C5OpsSwitch(int from, int to) {
    if (sC5Ops < 0) {
        sC5Ops = gdx3ds_config_get_int("debug", "c5ops", 0);
    }
    if (sC5Ops == 0) {
        return;
    }
    const uint32_t* op = sInterp->Stats().op;
    if (from == 9) {
        for (int i = 0; i < 256; i++) {
            sC5OpsWin[i] += op[i] - sC5OpsAt[i];
        }
    }
    if (to == 9) {
        memcpy(sC5OpsAt, op, sizeof(sC5OpsAt));
    }
}

/* Charge the open category and open `cat` (0 = close: the census goes dormant). */
GDX_COLD void C5CntSwitch(int cat) {
    const C5CntSnap now = C5CntTake();
    C5CntCharge(sC5CntCat, sC5CntAt, now);
    C5OpsSwitch(sC5CntCat, cat);
    sC5CntAt = now;
    sC5CntCat = cat;
}

/* C5 STAGE 3b [c5sub]: the same work counters plus a clock per SUB-category of the dressing
 * and effects (decomp p=0x62 markers, [debug] c5sub=1). A sub-category runs from its marker to
 * the next p=0x62 or p=0x61 marker or the walk's close; its time is the clock between them, so
 * it nests inside the [c5cat] category that holds it (dress= or fx=). One int test otherwise. */
constexpr int kC5SubCount = 18; /* GDX3DS_C5SUB_COUNT */
struct C5SubWin {
    C5CntWin w;
    uint64_t ticks;
    uint64_t visits;
};
int sC5Sub = 0; /* sub-category open; 0 = none */
uint64_t sC5SubT0 = 0;
C5CntSnap sC5SubAt = {};
C5SubWin sC5SubWin[kC5SubCount] = {};

void C5SubSwitch(int sub) {
    if (sC5Sub == 0 && sub == 0) {
        return;
    }
    const uint64_t now = static_cast<uint64_t>(gdx3ds_prof_now());
    const C5CntSnap snap = C5CntTake();
    if (sC5Sub > 0 && sC5Sub < kC5SubCount) {
        C5SubWin& win = sC5SubWin[sC5Sub];
        C5CntWin& w = win.w;
        w.cmds += snap.cmds - sC5SubAt.cmds;
        w.rects += snap.rects - sC5SubAt.rects;
        w.loads += snap.loads - sC5SubAt.loads;
        w.imports += snap.imports - sC5SubAt.imports;
        w.decodes += snap.decodes - sC5SubAt.decodes;
        w.batches += snap.batches - sC5SubAt.batches;
        w.draws += snap.draws - sC5SubAt.draws;
        w.triTicks += snap.triTicks - sC5SubAt.triTicks;
        w.impTicks += snap.impTicks - sC5SubAt.impTicks;
        w.drwTicks += snap.drwTicks - sC5SubAt.drwTicks;
        win.ticks += now - sC5SubT0;
        win.visits++;
    }
    sC5Sub = (sub > 0 && sub < kC5SubCount) ? sub : 0;
    sC5SubT0 = now;
    sC5SubAt = snap;
}

/* [native] skyclock: the [skycnt] census reads the [prof] sections through this. */
void SkyProfRead(uint64_t ticks[gdxn::Interp::kSkyProfSections], uint32_t calls[gdxn::Interp::kSkyProfSections]) {
    for (int i = 0; i < gdxn::Interp::kSkyProfSections && i < GDX3DS_PROF_SEC_COUNT; i++) {
        ticks[i] = gdx3ds_prof_sec_ticks[i];
        calls[i] = gdx3ds_prof_sec_calls[i];
    }
}

void C5CatMarker(uint32_t w0) {
    if (&gdx3ds_c5cat_marker != nullptr) {
        C5SubSwitch(0);
        gdx3ds_c5cat_marker(w0);
        if (sC5CntCat != 0) {
            C5CntSwitch(static_cast<int>(w0 & 0xFFu));
        }
    }
}

/* The task's walk opens the MISC category; the markers re-bucket it, and the close returns the
 * clock to the submit bucket it interrupted (stask). -1 = unarmed or no clock linked. */
int C5CatEnter(int cat) {
    if (&gdx3ds_c5cat_enter == nullptr) {
        return -1;
    }
    return gdx3ds_c5cat_enter(cat);
}

int C5CatOpenWalk() {
    const int prev = C5CatEnter(1 /* GDX3DS_C5CAT_MISC */);
    if (prev >= 0 && sInterp != nullptr) {
        sC5CntAt = C5CntTake();
        sC5CntCat = 1;
    }
    return prev;
}

void C5CatCloseWalk(int prev) {
    C5SubSwitch(0);
    if (prev >= 0 && &gdx3ds_c5cat_leave != nullptr) {
        if (sC5CntCat != 0) {
            C5CntSwitch(0);
        }
        gdx3ds_c5cat_leave(prev);
    }
}

/* C5 Stage 3b (port/3ds/gfx/gdx3ds_dress.h): the direct-load levels per scope kind -- [0] the
 * shadow / boost-flame replay runs and [3] the effects scope (fxshd_direct), [1] the gadget pass
 * (gadget_batch), [2] the decoration passes (dress_retained) -- and the decoration hook. Latched
 * once, when the interpreter is created. */
int sDressRetained = 0;

void DressLatch() {
    if (&gdx_dress_level == nullptr) {
        return;
    }
    const int fx = gdx_dress_level(3);
    const int levels[4] = { fx, gdx_dress_level(1), gdx_dress_level(2), fx };
    sDressRetained = levels[2];
    sInterp->DirectSetLevels(levels);
    char buf[200];
    snprintf(buf, sizeof(buf), "[dressd] init direct loads shd+fx=%d gadget=%d deco=%d | dress_retained=%d%s", fx,
             levels[1], levels[2], sDressRetained,
             ((fx == 2 || levels[1] == 2) && sHudDirect == 1) ? " (WARNING: verify legs want hud_direct 0 or 2: "
                                                                "one ledger, one mode)"
                                                              : "");
    RtLog(buf);
}

/* [native] nrr_hud (n60hud, gdx_nrr_hud.cpp): 0 off (default), 1 the NRR HUD decoder, 2 verify.
 * Not Tier-2. Acts inside HUD-kind hud_batch runs; refused (receipt) under interp_verify.
 * [native] nrr_hud_census=1: the [nrrhud] inventory lines (diagnostic, changes nothing drawn). */
int sNrrHud = 0;

/* n60finish [native] finish_mesh: the decomp's p=0x6A marker arms the G_DL hook for the very next
 * G_DL (Menus_DrawFinish's aFinishDL); any G_DL disarms it. Drawing thread only. */
bool sFinishArmed = false;

/* nimenu [native] ni_menu (gdx_nrr_menu.inc, gdx3ds_menu_capture.c): 0 off (default), 1 the non-race
 * modes' MENU-kind runs through the NRR program runner, 2 verify. The game side captures whenever the
 * ini asks for it (gdx3ds_hud_capture.c reads the same key); the runner needs the NRR HUD on. */
static void NiMenuLatch() {
    const int requested = gdx3ds_config_get_int("native", "ni_menu", 0);
    const int level = (requested < 0 || requested > 2) ? 0 : requested;
    sInterp->NiMenuSet(level);
    sInterp->NiMenuVolDiagSet(gdx3ds_config_get_int("debug", "nimenu_voldiag", 0)); /* menus-s2: diagnostic */
    /* menus-s2b [native] ni_menu_split (default 0): bits 1 FILLRECT chains, 2 texture loads -- a MENU run
     * with no program runs as segments (gdx_nrr_menu.inc) */
    const int split = gdx3ds_config_get_int("native", "ni_menu_split", 0);
    sInterp->NiMenuSplitSet(level == 1 ? split : 0);
    /* menus-s2b [native] ni_menu_keep (default 0): a volatile MENU run never empties the memo / pool */
    const int keep = gdx3ds_config_get_int("native", "ni_menu_keep", 0);
    sInterp->NiMenuKeepSet(level == 1 ? keep : 0);
    char ml[256];
    snprintf(ml, sizeof(ml),
             "[nimenu] init ni_menu=%d (requested %d) nrr_hud=%d kept=%d split=%d (requested %d) keep=%d (requested %d)%s",
             level, requested, sNrrHud, gdx3ds_config_get_int("native", "nrr_hud_kept", 0),
             level == 1 ? (split & 3) : 0, split, (level == 1 && keep == 1) ? 1 : 0, keep,
             (level != 0 && sNrrHud == 0) ? " (runner inert: needs nrr_hud; MENU runs replay stock)" : "");
    RtLog(ml);
}

/* nimach [native] ni_mach bit 2: the decomp's p=0x69 marker (gdx3ds_nrr_mach.h GDX_NIMACH_MODEL_P) arms
 * the hook for the very next G_DL -- a machine settings reflection model (D_800CDAD8), drawn as a
 * retained mesh entry (the dress registry: register on first sight, bytes re-checked every draw).
 * Any G_DL disarms it. Drawing thread only. */
bool sNimachModelArmed = false;

void NrrHudLatch() {
    sNrrHud = gdx3ds_config_get_int("native", "nrr_hud", 0);
    const int requested = sNrrHud;
    const int census = gdx3ds_config_get_int("native", "nrr_hud_census", 0);
    if (sNrrHud < 0 || sNrrHud > 2 || sVerifyOn) {
        sNrrHud = 0;
    }
    /* the NRR HUD draws through the NRR core: it needs [native] nrr=1 (and the core's own refusals) */
    const int core = gdx3ds_nrr_level(NRR_CAT_HUD);
    if (sNrrHud != 0 && core == 0) {
        sNrrHud = 0;
    }
    /* [native] nrr_hud_wide (n60hud3, default 7), bits: 1 a LOADBLOCK under a SETTIMG width > 1
     * (the font glyphs of the finish / results / name-entry screens) is kept virtual like the
     * width-1 form, its line pitch recorded; 2 an atlas view's core state is keyed by its page
     * (one state for a screen of glyphs); 4 a segmented SETTIMG operand is kept virtual too,
     * resolved when recorded. 0 = n60hud2's decoder (those rectangles draw the stock way). */
    const int wide = gdx3ds_config_get_int("native", "nrr_hud_wide", 7);
    sInterp->NrrHudSet(sNrrHud, census, gdx3ds_config_get_int("native", "nrr_hud_clock", 0), wide);
    sInterp->NrrHudMemoSet(sNrrHud == 1 ? gdx3ds_config_get_int("native", "nrr_hud_memo", 0) : 0); /* n60gp */
    sInterp->NrrHudDepDiagSet(gdx3ds_config_get_int("debug", "nrr_hud_depdiag", 0)); /* n60intro: diagnostic lines */
    /* [native] nrr_hud_pd (o3ds-cpucuts, default 1): a prim-depth word is a COL op (gdx_nrr_hud.cpp) */
    gdx3ds_nrr_hud_pd_set(gdx3ds_config_get_int("native", "nrr_hud_pd", 1));
    /* [native] nrr_hud_late (n60intro, default 0): programs that survive the race intro's sliding
     * boxes -- bit 1 a program rectangle takes the live scissor (a block variant), bit 2 a wide load's
     * pitch is checked when it runs instead of every other tile being an entry dependency, bit 4 an
     * interpreter batch open at a run's start is drawn first (NrrHudOpen's flush) so its program runs. */
    const int late = sNrrHud == 1 ? gdx3ds_config_get_int("native", "nrr_hud_late", 0) & 7 : 0;
    sInterp->NrrHudLateSet(late);
    /* [native] nrr_hud_kept (n60hudx, default 0): the capture records the G_DLs to the HUD's static setup
     * lists inside the runs (it reads the lever itself, and needs the NRR core's HUD level != 0); here the
     * NRR compiles them into the programs, and any run the NRR does not take walks them in place. Same
     * rule as the capture (gdx3ds_hud_capture.c HudKeptInit, n3ds): the ini lever AND the core's level,
     * whatever this side's nrr_hud became (verify mode included). */
    const int kept = gdx3ds_config_get_int("native", "nrr_hud_kept", 0) != 0 && core != 0 ? 1 : 0;
    sInterp->NrrHudKeptSet(kept);
    sInterp->NrrHudRunClockSet(gdx3ds_config_get_int("debug", "nrr_hud_runclock", 0)); /* n60hudx diagnostic */
    /* [native] nrr_hud_rr (n60hudx, default 0): unchanged runs replay their last output (bits: 1 runs of
     * >= 8 rectangles, 2 every run, 4 verify) */
    const int rr = sNrrHud == 1 ? gdx3ds_config_get_int("native", "nrr_hud_rr", 0) : 0;
    sInterp->NrrHudRrSet(rr);
    NiMenuLatch();
    char hl[240];
    snprintf(hl, sizeof(hl), "[nrrhud] init nrr_hud=%d (requested %d) core=%d census=%d wide=%d late=%d kept=%d rr=%d hud_batch=%d%s",
             sNrrHud, requested, core, census, wide, late, kept, rr, gdx3ds_config_get_int("native", "hud_batch", 0),
             (sNrrHud != 0 && gdx3ds_config_get_int("native", "hud_batch", 0) == 0) ? " (inert: needs hud_batch)"
                                                                                    : "");
    RtLog(hl);
}

void OwnedNoopHookBody(void* user, uint32_t w0, uint32_t w1, gdxn::Interp* interp);

/* [debug] nicensus: the context a marker's work is attributed to (-1: stays in the walk). */
GDX_COLD int NcMarkerCtx(uint32_t p) {
    if (p == GDX3DS_NRR_MARKER_P || p == GDX_NRRSKY_LEARN_P || p == GDX_NRRFX_LEARN_P || p == GDX_NRRDECO_REC_P) {
        return gdxn::Interp::NC_NRR;
    }
    if (p == 0x5E || p == 0x5F) {
        return gdxn::Interp::NC_HUD;
    }
    if (p == 0x5A || p == 0x5B || p == 0x60 || p == 0x5C || p == 0x5D || p == 0x6D || p == 0x6E || p == 0x65) {
        return gdxn::Interp::NC_SEAM;
    }
    if (p == 0x6B) {
        return gdxn::Interp::NC_MESH; /* nicourse [native] ni_course: the course preview's resident meshes */
    }
    return -1;
}

GDX_COLD void OwnedNoopHookCensus(void* user, uint32_t w0, uint32_t w1, gdxn::Interp* interp) {
    const uint32_t p = (w0 >> 16) & 0xFFu;
    const int ctx = NcMarkerCtx(p);
    const int prev = ctx >= 0 ? interp->NcSwitch(ctx) : -1;
    /* [debug] ni_strict: the context's tag is the marker (an NRR marker: 0xC0 | its category) */
    const int prevTag = ctx >= 0 ? interp->NcTag(p == GDX3DS_NRR_MARKER_P ? static_cast<int>(0xC0u | (w0 & 0x0Fu))
                                                                            : static_cast<int>(p))
                                 : -1;
    OwnedNoopHookBody(user, w0, w1, interp);
    if (prev >= 0) {
        interp->NcSwitch(prev);
        interp->NcTag(prevTag);
    }
}

void OwnedNoopHook(void* user, uint32_t w0, uint32_t w1, gdxn::Interp* interp) {
    if (GDX_UNLIKELY(sNicensus != 0)) {
        OwnedNoopHookCensus(user, w0, w1, interp);
        return;
    }
    OwnedNoopHookBody(user, w0, w1, interp);
}

void OwnedNoopHookBody(void* user, uint32_t w0, uint32_t w1, gdxn::Interp* interp) {
    GdxInterpSeam* seam = static_cast<GdxInterpSeam*>(user);
    const uint32_t p = (w0 >> 16) & 0xFFu;
    if (p == 0x5A || p == 0x5B || p == 0x60) { /* 0x60: [native] track_retained (C5 Stage 3) */
        gdx3ds_track_marker_seam(*seam, w0, static_cast<uintptr_t>(w1));
    } else if (p == 0x5C || p == 0x5D) {
        gdx3ds_fx_marker_seam(*seam, w0, static_cast<uintptr_t>(w1));
    } else if (p == 0x5E || p == 0x5F) {
        /* [native] hud_batch / fx_batch (0x5E), shadow_batch (0x5F): a compacted run
         * (gdx3ds_hud_capture.c) replays here. */
        interp->HudReplayRun(reinterpret_cast<const void*>(static_cast<uintptr_t>(w1)), w0 & 0xFFFFu);
    } else if (p == 0x61) {
        C5CatMarker(w0); /* decomp-3ds-c5cat-markers.patch: scene category boundary */
        if (&gdx3ds_gpucensus_marker != nullptr) {
            gdx3ds_gpucensus_marker(w0 & 0xFFu); /* n60gpu [debug] gpucensus: category from this list word */
        }
        gdx3ds_skylate_marker(w0 & 0xFFu, seam); /* n60tail [native] sky_late: list boundaries, the reorder */
        gdx3ds_overlap_scene_marker(w0 & 0xFFu); /* C5 Stage 4 [overlap]: maybe kick the GX queue */
        if (gdx3ds_replay_on) {
            gdx3ds_replay_scene_marker(w0 & 0xFFu); /* C6 [replay]: sky rectangles get the scroll patch */
        }
    } else if (p == 0x62) {
        C5SubSwitch(static_cast<int>(w0 & 0xFFu)); /* decomp-3ds-c5sub-markers.patch ([debug] c5sub) */
    } else if (p == 0x6C) {
        interp->DirectScope(static_cast<int>(w0 & 0xFFu)); /* C5 Stage 3b direct-load scope (gdx3ds_dress.h) */
    } else if (p == GDX_NIMACH_MODEL_P) {
        sNimachModelArmed = true; /* nimach: the next G_DL is a machine model (reflection pass) */
    } else if (p == 0x6A) {
        sFinishArmed = true; /* n60finish [native] finish_mesh: the next G_DL is the FINISH banner */
    } else if (p == 0x6B) {
        interp->NrrCourseHotMarker(w0, w1, seam); /* nicourse [native] ni_course: a course preview model */
    } else if (p == 0x6D || p == 0x6E) {
        gdx3ds_gad_marker_seam(*seam, w0, static_cast<uintptr_t>(w1)); /* [native] gadget_batch run */
    } else if (p == 0x65) {
        interp->SkyMarker(w0, w1); /* [native] sky_retain / skyclock (gdx_sky_retain.cpp) */
    } else if (p == GDX3DS_NRR_MARKER_P) {
        gdx3ds_nrr_marker(seam, w0, w1); /* [native] nrr: an NRR category draws (nrr/gdx3ds_nrr.h) */
    } else if (p == GDX_NRRSKY_LEARN_P) {
        interp->NrrSkyMarker(w0, w1); /* [native] nrr_sky: a learn region (gdx_nrr_sky.cpp, cold) */
    } else if (p == GDX_NRRFX_LEARN_P) {
        interp->NrrFxMarker(w0, w1); /* [native] nrr_shd / nrr_fx: a learn point (gdx_nrr_fx.cpp, cold) */
    } else if (p == GDX_NRRDECO_REC_P) {
        gdx3ds_nrr_deco_rec_marker(w0, w1); /* [native] nrr_deco: a decoration's record window (cold) */
    }
}

/* == gfx_dl_handler_common's gdx3ds_mesh_dl_hook call: a registered machine body list
 * reaching G_DL is drawn natively and the walker skips it. No bridge conversion runs under
 * interp=1, so the hook meets the game's own list and the mesh bakes lazily here, with the
 * walker's G_VTX resolution standing in for the bridge's. */
/* [native] machines_speck (the mesh module latches the ini with its other keys). */
bool SpeckNative() {
    return gdx3ds_mesh_speck_enabled() != 0;
}

/* The far speck lives in an asset image as 8-byte host-endian packets; the bake walker reads
 * host-built 16-byte packets. Re-pack it once per resolved address (an o2r asset image is
 * never rewritten once published) with the same command words the walker reads (ReadCmd).
 * NULL = not a shape the native path takes (big-endian or legacy-F3D source, no G_ENDDL). */
constexpr size_t kSpeckMaxCmds = 16;
struct SpeckPackets {
    const uint8_t* key;
    bool ok;
    alignas(8) uint8_t pk[kSpeckMaxCmds * 16];
};
SpeckPackets sSpeckPk = {};

const void* SpeckHostPackets(const gdxn::ListRef& l) {
    if (sSpeckPk.key == l.base) {
        return sSpeckPk.ok ? sSpeckPk.pk : nullptr;
    }
    sSpeckPk.key = l.base;
    sSpeckPk.ok = false;
    if (l.isBig || l.isF3d) {
        return nullptr;
    }
    memset(sSpeckPk.pk, 0, sizeof(sSpeckPk.pk));
    for (size_t i = 0; i < l.limit && i < kSpeckMaxCmds; i++) {
        const gdxn::Cmd c = gdxn::ReadCmd(l, i);
        uint8_t* p = sSpeckPk.pk + i * 16u;
        const uint64_t w1 = (static_cast<uint64_t>(c.w1hi) << 32) | c.w1;
        memcpy(p, &c.w0, 4); /* little-endian host: the walker's byte order */
        memcpy(p + 8, &w1, 8);
        if ((c.w0 >> 24) == 0xDFu) { /* G_ENDDL */
            sSpeckPk.ok = true;
            break;
        }
    }
    return sSpeckPk.ok ? sSpeckPk.pk : nullptr;
}

int OwnedDlHookBody(void* user, const void* listPtr, gdxn::Interp* interp);

GDX_COLD int OwnedDlHookCensus(void* user, const void* listPtr, gdxn::Interp* interp) {
    const int prev = interp->NcSwitch(gdxn::Interp::NC_MESH); /* [debug] nicensus */
    const int prevTag = interp->NcTag(0xDE);                    /* [debug] ni_strict: the G_DL hook */
    const int taken = OwnedDlHookBody(user, listPtr, interp);
    interp->NcSwitch(prev);
    interp->NcTag(prevTag);
    return taken;
}

int OwnedDlHook(void* user, const void* listPtr, gdxn::Interp* interp) {
    if (GDX_UNLIKELY(sNicensus != 0)) {
        return OwnedDlHookCensus(user, listPtr, interp);
    }
    return OwnedDlHookBody(user, listPtr, interp);
}

int OwnedDlHookBody(void* user, const void* listPtr, gdxn::Interp* interp) {
    GdxInterpSeam* seam = static_cast<GdxInterpSeam*>(user);
    if (interp->TexSkipOnDl(listPtr) != 0) {
        return 1; /* [native] machines_texskip: a skipped texture-set list (gdx_texskip.cpp) */
    }
    GdxOwnedSeamSetListHint(seam, listPtr);
    if (GDX_UNLIKELY(sFinishArmed)) {
        /* n60finish [native] finish_mesh: the G_DL right after the p=0x6A marker (aFinishDL). */
        sFinishArmed = false;
        const gdxn::ListRef& t = interp->DlHookList();
        if (!t.isF3d && gdx3ds_mesh_finish_hook_seam(*seam, listPtr, t.limit, t.stride, t.isBig ? 1 : 0,
                                                     GdxOwnedSeamResolveVtx, seam) != 0) {
            interp->TexSkipAfterDl(listPtr);
            return 1;
        }
    }
    if (GDX_UNLIKELY(sNimachModelArmed)) {
        sNimachModelArmed = false;
        const gdxn::ListRef& t = interp->DlHookList();
        if (!t.isF3d && gdx3ds_mesh_dress_model_ok(listPtr) != 0 &&
            gdx3ds_mesh_dress_hook_seam(*seam, listPtr, t.limit, t.stride, t.isBig ? 1 : 0, GdxOwnedSeamResolveVtx,
                                        seam) != 0) {
            interp->TexSkipAfterDl(listPtr);
            return 1;
        }
    }
    if ((interp->DirectScopeKind() == 2 && sDressRetained != 0) || gdx3ds_nrr_deco_arm != 0) {
        /* [native] dress_retained (C5 Stage 3b): a decoration body on the machine registry; [native]
         * nrr_deco: the same inside a decoration's record window (its draw is the record pass). */
        const gdxn::ListRef& t = interp->DlHookList();
        if (!t.isF3d && gdx3ds_mesh_dress_hook_seam(*seam, listPtr, t.limit, t.stride, t.isBig ? 1 : 0, GdxOwnedSeamResolveVtx,
                                        seam) != 0) {
            interp->TexSkipAfterDl(listPtr);
            return 1;
        }
    }
    int taken = 0;
    if (SpeckNative() && interp->DlOperandIsSpeck(listPtr)) {
        /* [native] machines_speck (c5mach2): the far speck drawn as a native mesh. */
        const void* packets = SpeckHostPackets(interp->DlHookList());
        if (packets != nullptr) {
            taken = gdx3ds_mesh_speck_hook_seam(*seam, listPtr, packets, GdxOwnedSeamResolveVtx, seam);
        }
    }
    if (taken == 0) {
        taken = gdx3ds_mesh_dl_hook_seam(*seam, listPtr, GdxOwnedSeamResolveVtx, seam);
    }
    interp->TexSkipAfterDl(listPtr);
    return taken;
}

/* [native] machines_texskip's view of the machine registry (gdx3ds_mesh.cpp). The body probe
 * resolves the list's vertices exactly as the G_DL hook will, so it sets the same list hint. */
int TexSkipBodyProbe(void* user, const void* body, uint8_t inhTile, uint8_t inhUse, int* slot, uint8_t* tiles) {
    GdxInterpSeam* seam = static_cast<GdxInterpSeam*>(user);
    GdxOwnedSeamSetListHint(seam, body);
    return gdx3ds_mesh_body_probe(body, GdxOwnedSeamResolveVtx, seam, inhTile, inhUse, slot, tiles);
}
int sTexSkip = 0; /* [native] machines_texskip: 0 off, 1 skip, 2 verify */
uint32_t sTasksSinceReceipt = 0;
uint32_t sVerifyMismatches = 0;  /* tasks with a per-batch mismatch */
/* Mismatching tasks split by class, and by the state sub-field that diverged first. The single
 * "first mismatch" detail dump is capped, and a screen that mismatches early (the title) used
 * the whole budget before a later screen was ever reached, so the counters carry the shape of
 * the run and the detail budget is now per receipt window instead of per run. */
uint32_t sVfyClsKind = 0, sVfyClsState = 0, sVfyClsVbo = 0, sVfyClsExtra = 0, sVfyClsMissing = 0;
uint32_t sVfyFieldHist[12] = {};
uint32_t sVerifyDetailWindow = 0;
constexpr uint32_t kVerifyDetailPerWindow = 2;
uint32_t sVerifyChecked = 0;
uint32_t sHistMismatches = 0;    /* M0 opcode-histogram receipt only (the bridge skips wide-cache hits) */

/* nirace [native] ni_race (gdx_nirace.cpp; nirace-progress.md): 0 off (default), 1 the race frame's
 * inter-marker spans run as compiled span programs, 2 verify (each compiled register group checked
 * against its stock handlers; the stock result stays). Not Tier-2. Refused under interp_verify.
 * Armed per task in a 1P race scope (GP, practice, time attack, death race), and in VS 2P-4P with
 * [native] ni_view's NRR_NI_VIEW_RACE bit (native-vs). [debug] nirace_rcpt=N:
 * the [nirace] receipt every N race-scope frames (default 256 with the lever on; on its own it
 * prints the walker's commands per frame with the lever off -- the "before" number). */
int sNiRace = 0;
int sNiRaceReq = 0;
/* menus-s2 [native] ni_menu_root (native-all-plan.md S2(a)): 0 off (default), 1 the span programs also
 * run the root list of a non-race MENU frame -- the frame root every screen walks (the segment table,
 * the colour / z image, the setup lists and matrices between the MENU runs), the ~57-110 cmds/f the
 * menus had left after ni_menu. Same programs, same exactness argument as ni_race (every op is the
 * stock handler on the live word or a checked store set); the level (verify) is ni_race's when that is
 * on. Needs [native] ni_menu (the MENU runs are the spans' markers). Expansion Kit editor modes stay
 * walked (their own track). */
int sNiMenuRoot = 0;
int sNiMenuRootReq = 0;
/* menus-s4 [native] ni_menu_xition (native-all-plan.md S4; default 0, read only with ni_menu=1, refused
 * under interp_verify): the menu transitions. On the render side, the mode-change frames -- the hide
 * half of every wipe runs while gGameModeChangeState != GAMEMODE_UPDATE, and so does the one
 * GAMEMODE_CHANGE_INIT frame between hide and appear -- keep the ni_menu_root span programs (the
 * root was armed on steady frames only, so every hide frame walked its ~57-67 root words and, under a
 * tiled wipe, 64 tiles' matrix / vertex / triangle words). The game side (gdx3ds_hud_capture.c
 * gdxmenu_capture_xition) reads the same key. [debug] xition_mask (default all) bisects the parts:
 * 1 change-frame roots, 2 the INIT frame's capture (game side). Not here: the tiled whirl / spiral's
 * 64 perspective tiles stay on the walker (VTX / TRI2 / their texture loads), and cutting a wipe's
 * rectangles into segments of their own was tried and dropped -- the moving rectangles' words change
 * every frame either way, so the cut segments decoded and replayed stock (records: 135 -> 412 / f). */
int sNiMenuXition = 0;
int sNiMenuXitionReq = 0;
int sNiMenuXitionMask = 0;
struct XitionWin {
    uint32_t tasks, wipe[16]; /* tasks in the window; tasks that drew transition type t */
    uint32_t changeRoot;      /* mode-change frames whose root ran as span programs */
    uint64_t walkedWipe;      /* the walker's commands on the transition frames */
};
XitionWin sXitionWin = {};
int sNiRaceMesh = 0; /* [native] ni_race_mesh (default 1): geometry segments drawn by the mesh path */
int sNiRaceVs = 0;   /* native-vs: ni_view bit NRR_NI_VIEW_RACE -- the span programs in VS 2P-4P frames too */
int sNiRaceFv = 0;   /* o3ds60: ni_fullview bit NRR_NI_VIEW_RACE -- the span programs in MP fullview frames */
int sNiRcpt = 0;
struct NiWin {
    uint32_t frames[3];  /* [0] racing, [1] the rest of the race scope, [2] menu frames (ni_menu_root) */
    uint64_t walked[3];  /* the walker's commands (Counters::cmds) of those frames */
};
NiWin sNiWin = {};
uint32_t sNiWinFrames = 0;
/* nointerp 2026-10-15: the combined (nirace+nimach+nimenu+niview+nicourse) binary's nicensus
 * residue read ~1023 cmds/f on steady 1P racing where standalone nirace reads ~1.3 -- "the runner
 * doesn't arm" was one of the two live hypotheses (nointerp-progress.md). A `[niarm]` arm-window
 * counter (NiArmWin, acbb081c) answered it without needing to reproduce the combined tree here:
 * `armed` tracked nicensus's own racing-frame count exactly (nirace-progress.md 4R4), so arming was
 * cleared and the real residue (NrrRaceHotDrive's first-span-bail-and-never-retry) was root-caused
 * and fixed separately. Question closed; the counters themselves removed here (W6, Fable review). */

GDX_COLD void NiRaceLatch() {
    sNiRaceReq = gdx3ds_config_get_int("native", "ni_race", 0);
    sNiRace = sNiRaceReq;
    if (sNiRace < 0 || sNiRace > 2 || sVerifyOn) {
        sNiRace = 0;
    }
    sNiMenuRootReq = gdx3ds_config_get_int("native", "ni_menu_root", 0);
    sNiMenuRoot = (sNiMenuRootReq == 1 && !sVerifyOn && gdx3ds_config_get_int("native", "ni_menu", 0) != 0) ? 1 : 0;
    sNiMenuXitionReq = gdx3ds_config_get_int("native", "ni_menu_xition", 0);
    sNiMenuXition = (sNiMenuXitionReq == 1 && !sVerifyOn && gdx3ds_config_get_int("native", "ni_menu", 0) == 1) ? 1 : 0;
    sNiMenuXitionMask = sNiMenuXition != 0 ? gdx3ds_config_get_int("debug", "xition_mask", 0xFF) : 0;
    sNiRcpt = gdx3ds_config_get_int("debug", "nirace_rcpt", (sNiRace != 0 || sNiMenuRoot != 0) ? 256 : 0);
    /* round 7 (nirace-progress.md 4R7): default ON now that the stereo-replay refusal gates the
     * one known-unsafe case -- 2D matrix exact, ni_race=2 verify bad=0, 3D matrix byte-identical
     * to mesh=0, all under the fix. (The refusal itself moved from a per-task-begin latch,
     * `meshRefuseStereo`, to a live per-dispatch read of `gdx3ds_stereo_replay_frame` -- nimenu
     * W1, review finding H1; the field this comment used to name is gone.) */
    sNiRaceMesh = sNiRace != 0 ? (gdx3ds_config_get_int("native", "ni_race_mesh", 1) != 0 ? 1 : 0) : 0;
    sNiRaceVs = (sNiRace != 0 && gdx3ds_nrr_ni_view_race() != 0) ? 1 : 0;
    sNiRaceFv = (sNiRace != 0 && sNiRaceVs == 0 && gdx3ds_nrr_ni_fullview_race() != 0) ? 1 : 0;
}

/* o3ds60: the VS frame's race runs MP fullview (game/gdx3ds_fullview_game.c, read live like the race
 * scope beside it). A frame at a fullview edge that reads the other way only runs span programs that
 * check every word they were compiled from, so it stays exact. */
extern "C" int gdx_fv_live_k(void) __attribute__((weak));
static GDX_COLD bool NiRaceFullviewFrame() {
    return sNiRaceFv != 0 && &gdx_fv_live_k != nullptr && gdx_fv_live_k() >= 0;
}

/* nirace [native] ni_race_mesh: a span program's geometry segment (G_VTX words + a triangle run, narrow
 * packets) drawn by the machine registry's dress entries -- the path dress_retained / nrr_deco bodies
 * take (bake once, the interpreter's own T&L / clip / bind rules). The vertices resolve against the
 * list the words came from. 1 = drawn. */
int NiSegHook(void* user, const void* seg, size_t cmds, const void* hint) {
    GdxInterpSeam* seam = static_cast<GdxInterpSeam*>(user);
    GdxOwnedSeamSetListHint(seam, hint);
    return gdx3ds_mesh_dress_hook_seam(*seam, seg, cmds, 8, 0, GdxOwnedSeamResolveVtx, seam);
}

/* [debug] ni_strict: 0 off (default), 1 count, 2 drop = TODO (counts). Raises the nicensus engine;
 * sNicensusDumps keeps whether [nicensus]'s own receipts were asked for. */
int sNicensusDumps = 0;
int sNiStrictReq = 0;
GDX_COLD void NiStrictLatch() {
    sNicensusDumps = sNicensus;
    sNiStrictReq = gdx3ds_config_get_int("debug", "ni_strict", 0);
    sNiStrict = (sNiStrictReq == 1 || sNiStrictReq == 2) ? 1 : 0;
    sNiStrictEvery = gdx3ds_config_get_int("debug", "ni_strict_every", 3600);
    if (sNiStrict != 0) {
        sNicensus = 1;
    }
}

void Latch() {
    if (sLatched) {
        return;
    }
    if (!gdx3ds_config_loaded()) {
        return; /* defer: keep compiled-in defaults until the ini has been read */
    }
    sLatched = 1;
    sInterpOn = gdx3ds_native_lever("interp", 0) != 0; /* always explicit: [native] all is Tier-2 only (gdx3ds_native_cfg.h) */
    sVerifyOn = gdx3ds_config_get_int("native", "interp_verify", 0) != 0;
    sCpuClip = gdx3ds_config_get_int("native", "cpuclip", 1) != 0; /* LUS parity default; 0 = PICA-only clip (M2 A/B) */
    sBlendFlush = gdx3ds_config_get_int("native", "blendflush", 1) != 0;
    sCensus = gdx3ds_config_get_int("native", "ncensus", 0) != 0; /* raw: a diagnostic, not a path */
    sDspCensus = gdx3ds_config_get_int("native", "dspcensus", 0);
    sNicensus = gdx3ds_config_get_int("debug", "nicensus", 0) != 0 ? 1 : 0;
    NiRaceLatch(); /* nirace [native] ni_race */
    sNicensusEvery = gdx3ds_config_get_int("debug", "nicensus_every", 1800);
    sNicensusWipe = gdx3ds_config_get_int("debug", "nicensus_wipe", 0) != 0 ? 1 : 0; /* menus-s4 */
    NiStrictLatch(); /* [debug] ni_strict */
    /* NOT Tier-2 (it is absent from kTier2Levers in gdx3ds_native_cfg.c), so `all=1` neither
       turns it on nor off -- it now defaults ON in its own right, and `all=1 interp=1` is one
       of the two configurations the corpus gate runs it in. An explicit `timgmemo = 0` is
       still the killswitch; the Tier-2 master has never had any say over this lever. */
    sTimgMemo = gdx3ds_native_lever("timgmemo", 1) != 0;
    /* Not Tier-2 and not a boolean: tmemskip is a LEVEL (see sTmemSkip). */
    sTmemSkip = gdx3ds_config_get_int("native", "tmemskip", 0);
    /* Not Tier-2: `all=1` never turns it on (C5 Stage 1, default 0 until the hardware gate). */
    sTexSkip = gdx3ds_config_get_int("native", "machines_texskip", 0);
    sTmemCls = gdx3ds_config_get_int("native", "tmemcls", 0);
    sHudCensus = gdx3ds_config_get_int("native", "hudcensus", 0) != 0; /* [hudop], raw diagnostic */
    sHudDirect = gdx3ds_config_get_int("native", "hud_direct", 0);
    if (sHudDirect < 0 || sHudDirect > 2) {
        sHudDirect = 0;
    }
    if (sHudDirect != 0 && sVerifyOn) {
        sHudDirect = 0; /* the interp_verify oracle turns the HUD capture off anyway */
    }
    sHudRetain = gdx3ds_config_get_int("native", "hud_retain", 0);
    if (sHudRetain < 0 || sHudRetain > 2 || sHudDirect != 1) {
        sHudRetain = 0; /* acts inside direct HUD runs only (and hud_direct is 0 under interp_verify) */
    }
    {
        char hl[160];
        const int hudBatch = gdx3ds_config_get_int("native", "hud_batch", 0);
        snprintf(hl, sizeof(hl), "[hudd] init hud_direct=%d hud_batch=%d%s", sHudDirect, hudBatch,
                 (sHudDirect != 0 && hudBatch == 0) ? " (inert: hud_direct acts inside the hud_batch replay)" : "");
        RtLog(hl);
        snprintf(hl, sizeof(hl), "[hudr] init hud_retain=%d (requested %d) hud_direct=%d hud_batch=%d%s", sHudRetain,
                 gdx3ds_config_get_int("native", "hud_retain", 0), sHudDirect, hudBatch,
                 (sHudRetain != 0 && hudBatch == 0) ? " (inert: needs hud_batch)" : "");
        RtLog(hl);
    }
    /* [native] hudsky_emit (hudsky): 0 off, 1 the compact emitter, 2 verify. Acts with hud_retain=1
       (the retained runs it compiles) and inside sky runs (their drains); not Tier-2. */
    sHudSkyEmit = gdx3ds_config_get_int("native", "hudsky_emit", 0);
    if (sHudSkyEmit < 0 || sHudSkyEmit > 2 || sVerifyOn) {
        sHudSkyEmit = 0;
    }
    {
        char hl[200];
        snprintf(hl, sizeof(hl), "[hsx] init hudsky_emit=%d (requested %d) hud_retain=%d sky_retain=%d%s", sHudSkyEmit,
                 gdx3ds_config_get_int("native", "hudsky_emit", 0), sHudRetain,
                 gdx3ds_config_get_int("native", "sky_retain", 0),
                 (sHudSkyEmit != 0 && sHudRetain != 1) ? " (HUD half inert: needs hud_retain=1)" : "");
        RtLog(hl);
    }
    sSkyRetain = gdx3ds_config_get_int("native", "sky_retain", 0);
    sSkyClock = gdx3ds_config_get_int("native", "skyclock", 0);
    if (sSkyClock < 0 || sSkyClock > 2) {
        sSkyClock = 0; /* 1 the [skycnt] census, 2 also the [skyop] per-opcode clock */
    }
    if (sSkyRetain < 0 || sSkyRetain > 3 || sVerifyOn) {
        sSkyRetain = 0; /* the capture is off under interp_verify too (gdx3ds_sky_capture.c) */
    }
    if (sVerifyOn) {
        sSkyClock = 0;
    }
    sTmemFault = gdx3ds_config_get_int("debug", "tmemfault", 0);
    sO3dsFast = gdx3ds_config_get_int("native", "o3dsfast", 1) != 0 ? 1 : 0;
    sTriMemo = gdx3ds_config_get_int("native", "trimemo", 0) != 0 ? 1 : 0;
#ifdef EXPANSION_KIT
    sEkWidgetScissor = gdx3ds_config_get_int("debug", "ek_widget_scissor", 1) != 0;
    sEkEditMesh = gdx3ds_config_get_int("native", "ek_editmesh", 0) != 0 ? 1 : 0;
    if (sEkEditMesh != 0 && gdx3ds_config_get_int("native", "ek_editmesh_verify", 0) != 0) {
        sEkEditMesh = 2;
    }
#endif
    /* ownfold IS Tier-2 (`all=1` turns it on, exactly as it turns on fx_hud -- they are the
       same fold, one per interpreter); an explicit `ownfold = 0` still wins. */
    sOwnFold = gdx3ds_native_lever("ownfold", 0) != 0;
    /* palhash and texhash_ci were found independently by two agents for the same B9 failure and
       fix the same class from different ends: palhash folds the live TLUT into the CI key,
       texhash_ci content-keys the CI/RGBA32 classes. Both stay: texinval is additionally the
       only thing honouring gdx_rdram_mode_reset and minimap.c on the owned path. */
    sPalHash = gdx3ds_config_get_int("native", "palhash", 1) != 0;
    sTexHashCi = gdx3ds_config_get_int("native", "texhash_ci", 1) != 0;
    sTexInval = gdx3ds_config_get_int("native", "texinval", 1) != 0;
    /* DROPSITES. Two killswitches (default = the fix) and two forcing knobs (default = shipped).
     * texstalefix=0 restores "an import that decoded nothing is still marked uploaded", the arm
     * that leaves a recycled backend id's previous pixels cached under the new key.
     * vtxclamp=0 restores "an over-range G_VTX loads nothing at all".
     * [debug] texscratchkib caps the decode scratch's growth ceiling; [debug] vtxmax shrinks the
     * effective G_VTX window. Both exist to force the drop arms in a normal build. */
    sTexStaleFix = gdx3ds_config_get_int("native", "texstalefix", 1) != 0;
    sTexKeepKib = gdx3ds_config_get_int("debug", "tex_keep", 0) != 0
                      ? gdx3ds_config_get_int("debug", "tex_keep_kb", 2048)
                      : 0;
    sTexKeepFloorKib = gdx3ds_config_get_int("debug", "tex_keep_floor_kb", 3072);
    sVtxClamp = gdx3ds_config_get_int("native", "vtxclamp", 1) != 0;
    sTexScratchKib = gdx3ds_config_get_int("debug", "texscratchkib", 0);
    sTexScratchMaxKib = gdx3ds_config_get_int("debug", "texscratchmaxkib", 0);
    sVtxMax = gdx3ds_config_get_int("debug", "vtxmax", 0);
    sClipPolyMax = gdx3ds_config_get_int("debug", "clippolymax", 0);
    sRawCopyWhole = gdx3ds_config_get_int("native", "rawcopywhole", 1) != 0;
    /* The three memo levers ship default-ON as of native/tmemblock, so the receipt prints the
       RESOLVED value of all three side by side: timgmemo from this file's latch, dlfacts and
       stubgen read back out of the bridge through gdx_bridge_*(). Before the promotion a leg
       was identified by a number that only an explicit ini key could produce; now an absent
       key and an explicit `=1` agree, so a mistyped override would be invisible unless the
       receipt reports what resolved. Both accessors are weak: a build that links the glue
       without the bridge prints -1 rather than failing to link. */
    const int dlFactsResolved = (&gdx_bridge_dlfacts_on != nullptr) ? gdx_bridge_dlfacts_on() : -1;
    const int stubGenResolved =
        (&gdx_bridge_stubgen_narrow_on != nullptr) ? gdx_bridge_stubgen_narrow_on() : -1;
    /* [memotear] [native] memoorder -- the publication-order/barrier level of the six
       process-wide [brfast] memos. Same weak-accessor rule as the two above. */
    const int memoOrderResolved = (&gdx_bridge_memoorder != nullptr) ? gdx_bridge_memoorder() : -1;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "[native] interp=%d path=%s cpuclip=%d blendflush=%d palhash=%d texhash_ci=%d texinval=%d "
             "rawcopywhole=%d verify=%d ncensus=%d dspcensus=%d ownfold=%d timgmemo=%d dlfacts=%d stubgen=%d "
             "tmemskip=%d tmemcls=%d texstalefix=%d vtxclamp=%d memoorder=%d tmemfault=%d o3dsfast=%d "
             "trimemo=%d hooks=%s",
             sInterpOn, sInterpOn ? "owned" : "lus", sCpuClip, sBlendFlush, sPalHash, sTexHashCi,
             sTexInval, sRawCopyWhole, sVerifyOn, sCensus, sDspCensus, sOwnFold, sTimgMemo,
             dlFactsResolved, stubGenResolved, sTmemSkip, sTmemCls, sTexStaleFix, sVtxClamp,
             memoOrderResolved, sTmemFault, sO3dsFast, sTriMemo,
             sInterpOn ? "noop(track,fx)+dl(machines)" : "lus");
    RtLog(buf);
#ifdef EXPANSION_KIT
    /* Always-on receipt, one line, EK builds only (PROTOCOL: every feature behind an ini
     * killswitch WITH a receipt). RtLog reaches the filelog, which is where an agent reads
     * it -- ekdrive gotcha 3: gdx_cki and GDX_LOG have never printed on this platform. */
    {
        char ekbuf[96];
        snprintf(ekbuf, sizeof(ekbuf), "[b11] [ekwidget] scissor pillar-safe lever=%d", sEkWidgetScissor);
        RtLog(ekbuf);
    }
#endif
}

/* [dynlod] receipt (port/3ds/gdx3ds_dynlod.c). The LUS path prints it from the bridge's
 * [race-dl] census inside GdxGfxJobRun, which the owned walker never enters, so under interp=1
 * the controller's tier/raises/lowers were invisible ([wobble] audit). Weak like the bridge's. */
extern "C" int gdx3ds_dynlod_receipt(char* buf, int cap) __attribute__((weak));

/* [ncensus] — what ended each batch over the window, ranked by the caller. Two lines:
 *   [ncensus] tasks= batches= b/task= maxb/task= tris/batch= drained= | hist=<8 buckets>
 *   [ncensus] <class>=<drains>/<calls>@<ms> ... | foldp=<n>/<primDrains> folde=<n>/<envDrains>
 *             folded p=<n> e=<n>
 * `drains` = flushes that actually cut a non-empty batch (the split that costs a draw);
 * `calls` = every time the cause fired (an empty batch costs nothing); `ms` = time inside
 * those drains (state application + DrawPackedBatch) over the whole window, not per frame. */
void EmitCensus(const gdxn::Counters& s) {
    static const char* const kName[gdxn::FLUSH_COUNT] = { "none",  "shader", "texture", "depth",  "decal",
                                                          "vport", "scissor", "alpha",  "prim",   "env",
                                                          "fogcol", "blenda", "primdep", "fbop",  "poolfull",
                                                          "taskend", "fogline", "comb",  "sampler", "marker",
                                                          "pose" };
    char buf[400];
    const unsigned drained = [&] {
        unsigned n = 0;
        for (int i = 0; i < gdxn::FLUSH_COUNT; i++) n += s.flushDrain[i];
        return n;
    }();
    int n = snprintf(buf, sizeof(buf),
                     "[ncensus] tasks=%u batches=%u bpt=%u maxbpt=%u tpb=%u drained=%u | hist", s.tasks, s.batches,
                     s.tasks ? s.batches / s.tasks : 0u, s.maxBatchesPerTask,
                     s.batches ? s.batchTris / s.batches : 0u, drained);
    for (int i = 0; i < gdxn::kBatchHistBuckets && n > 0 && n < (int)sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - n, " %u", s.batchHist[i]);
    }
    RtLog(buf);
    n = snprintf(buf, sizeof(buf), "[ncensus] split");
    for (int i = 1; i < gdxn::FLUSH_COUNT && n > 0 && n < (int)sizeof(buf); i++) {
        if (s.flush[i] == 0 && s.flushDrain[i] == 0) continue;
        n += snprintf(buf + n, sizeof(buf) - n, " %s=%u/%u@%u", kName[i], s.flushDrain[i], s.flush[i],
                      (unsigned)((double)s.flushTicks[i] / CPU_TICKS_PER_MSEC * 1000.0)); /* us */
    }
    if (n > 0 && n < (int)sizeof(buf)) {
        snprintf(buf + n, sizeof(buf) - n, " | us | foldable p=%u e=%u folded p=%u e=%u", s.foldable[0], s.foldable[1],
                 s.folded[0], s.folded[1]);
    }
    RtLog(buf);
}

/* [dspop] -- the per-opcode SELF-time census of the owned walker ([native] dspcensus).
 * Two lines per receipt window:
 *   [dspop] tasks= cmds= walk=<ms> attributed=<ms> fetch=<ms>/<calls> loop=<ms> | top8 by self ms
 *   [dspop] ph dlresolve=<ms>/<calls> dlhook=.. dlpush=..            (dspcensus=2 only)
 * `walk` is the whole Walk() wall time minus nothing; `attributed` is the sum of the opcode
 * self buckets plus fetch; `loop` is the remainder (the while/stack/branch overhead plus the
 * census's own clock reads). Opcode ms are SELF -- the [prof] vtx/tri/imp/drw/mtx sections a
 * handler entered are already subtracted, so these are the milliseconds that live in `dsp`. */
void EmitDspCensus(const gdxn::Counters& s) {
    char buf[480];
    const double msPerTick = 1.0 / CPU_TICKS_PER_MSEC;
    uint64_t attributed = s.phTicks[gdxn::DSP_PH_FETCH];
    for (int i = 0; i < 256; i++) {
        attributed += s.opTicks[i];
    }
    const uint64_t loop = (s.walkTicks > attributed) ? (s.walkTicks - attributed) : 0;
    int n = snprintf(buf, sizeof(buf),
                     "[dspop] tasks=%u cmds=%u walk=%.2f attributed=%.2f fetch=%.2f/%u loop=%.2f | top",
                     s.tasks, s.cmds, (double)s.walkTicks * msPerTick, (double)attributed * msPerTick,
                     (double)s.phTicks[gdxn::DSP_PH_FETCH] * msPerTick, s.phCalls[gdxn::DSP_PH_FETCH],
                     (double)loop * msPerTick);
    /* Top 8 opcodes by self ticks, selection sort over a local copy (256 entries, once per
     * 64 tasks -- the receipt is not the hot path). */
    uint64_t t[256];
    memcpy(t, s.opTicks, sizeof(t));
    for (int rank = 0; rank < 8 && n > 0 && n < (int)sizeof(buf); rank++) {
        int best = -1;
        uint64_t bestTicks = 0;
        for (int op = 0; op < 256; op++) {
            if (t[op] > bestTicks) {
                bestTicks = t[op];
                best = op;
            }
        }
        if (best < 0) {
            break;
        }
        n += snprintf(buf + n, sizeof(buf) - n, " %02X=%.2f/%u", (unsigned)best, (double)bestTicks * msPerTick,
                      s.op[best]);
        t[best] = 0;
    }
    RtLog(buf);
    if (sDspCensus >= 2) {
        snprintf(buf, sizeof(buf),
                 "[dspop] ph dlresolve=%.2f/%u dlhook*=%.2f/%u dlpush=%.2f/%u | timgres=%.2f/%u "
                 "timgclass=%.2f/%u tmemsame=%.2f/%u tmemcopy=%.2f/%u tmemstore=%.2f/%u | skips=%u",
                 (double)s.phTicks[gdxn::DSP_PH_DLRESOLVE] * msPerTick, s.phCalls[gdxn::DSP_PH_DLRESOLVE],
                 (double)s.phTicks[gdxn::DSP_PH_DLHOOK] * msPerTick, s.phCalls[gdxn::DSP_PH_DLHOOK],
                 (double)s.phTicks[gdxn::DSP_PH_DLPUSH] * msPerTick, s.phCalls[gdxn::DSP_PH_DLPUSH],
                 (double)s.phTicks[gdxn::DSP_PH_TIMGRES] * msPerTick, s.phCalls[gdxn::DSP_PH_TIMGRES],
                 (double)s.phTicks[gdxn::DSP_PH_TIMGCLASS] * msPerTick, s.phCalls[gdxn::DSP_PH_TIMGCLASS],
                 (double)s.phTicks[gdxn::DSP_PH_TMEMSAME] * msPerTick, s.phCalls[gdxn::DSP_PH_TMEMSAME],
                 (double)s.phTicks[gdxn::DSP_PH_TMEMCOPY] * msPerTick, s.phCalls[gdxn::DSP_PH_TMEMCOPY],
                 (double)s.phTicks[gdxn::DSP_PH_TMEMSTORE] * msPerTick, s.phCalls[gdxn::DSP_PH_TMEMSTORE],
                 s.tmemSkips);
        RtLog(buf);
        /* dlhook is starred because it is NOT dispatch -- see DSP_PH_DLHOOK in gdx_interp.h.
           It is the machines DrawMesh billed to G_DL, and it is inclusive where every other
           phase on this line is self time. Printed once per census window so the annotation
           travels with the numbers into whatever log someone reads next. */
        RtLog("[dspop] note: dlhook* is the machines DrawMesh hook (real drawing, inclusive), "
              "not dispatch -- the dispatch part of G_DL is dlresolve");
    }
    /* The full per-opcode call table, so the owned walker's command mix can be diffed against
     * the other walker's without a second run. One line, non-zero entries only. */
    n = snprintf(buf, sizeof(buf), "[dspop] calls");
    for (int op = 0; op < 256 && n > 0 && n < (int)sizeof(buf) - 16; op++) {
        if (s.op[op] != 0) {
            n += snprintf(buf + n, sizeof(buf) - n, " %02X=%u", (unsigned)op, s.op[op]);
        }
    }
    RtLog(buf);
}

/* [dspx] + [trix] (o3dsinterp, NEXT.md C4m) -- emitted with every [ninterp] window while
 * gputrace ([prof]) is armed. Everything is per TASK (== per frame with renderthread=0, nRun=1).
 *
 *   [dspx] tasks= ms/task load=<ms>/<calls> dl= mch= trk= fx= rect= mv= sum= | cmds= lists=
 *          | KB/task copy= swap= spanw=<words>
 * SELF ms of each handler group inside `dsp` (DxGroup in gdx_interp.h), one calibrated clock
 * read (clk=) subtracted per call; `dsp - sum` from the
 * [prof] line of the same window is the per-command walk + the untimed state handlers. The KB
 * columns are the TMEM mirror's memory traffic (always counted).
 *
 *   [trix] tasks= /task calls= top= rect= vrej= clip= fan= allout= rej= cull= res= memo= begin= emit=
 *          | us/call screen= clip= resolve= begin= emit= | ms/task screen= clip= resolve= begin= emit=
 *          (n=<samples>)
 * Exit-path counts are exact. The phase clocks are sampled (1 top-level SpTri in 32) and each
 * closed interval has one calibrated clock read (clk=, an SVC) subtracted; us/call is a
 * phase's ticks over ALL samples (a sample that returned early contributes 0 to the later
 * phases), so ms/task = us/call * top / 1000 and the five ms/task add up to an estimate of
 * `tri` self time minus the ProfScope's own two clock reads per call. `res`/`memo` also count
 * the machines binder's ResolveDrawState calls (BindMaterial), which are not SpTri calls. */
/* The cost of one clock read (an SVC) in ticks, measured once: the [dspx]/[trix] intervals each
 * contain one read, so the receipt subtracts it rather than report the instrument. */
double ClockReadTicks() {
    static double sCost = -1.0;
    if (sCost < 0.0) {
        const int kReads = 256;
        const uint64_t t0 = static_cast<uint64_t>(gdx3ds_prof_now());
        uint64_t t = t0;
        for (int i = 0; i < kReads; i++) {
            t = static_cast<uint64_t>(gdx3ds_prof_now());
        }
        sCost = static_cast<double>(t - t0) / kReads;
    }
    return sCost;
}

void EmitDxTrix(const gdxn::Counters& s) {
    if (s.tasks == 0 || gdx3ds_prof_active == 0) {
        return;
    }
    const double clk = ClockReadTicks();
    char buf[400];
    const double msPerTick = 1.0 / CPU_TICKS_PER_MSEC;
    const double perTask = 1.0 / (double)s.tasks;
    static const char* const kDx[gdxn::DX_COUNT] = { "load", "dl", "mch", "trk", "fx", "rect", "mv" };
    int n = snprintf(buf, sizeof(buf), "[dspx] tasks=%u ms/task", s.tasks);
    uint64_t sum = 0;
    for (int g = 0; g < gdxn::DX_COUNT && n > 0 && n < (int)sizeof(buf); g++) {
        const double raw = (double)s.dxTicks[g] - clk * (double)s.dxCalls[g];
        const double net = raw > 0.0 ? raw : 0.0;
        sum += static_cast<uint64_t>(net);
        n += snprintf(buf + n, sizeof(buf) - n, " %s=%.2f/%u", kDx[g], net * msPerTick * perTask,
                      (unsigned)((double)s.dxCalls[g] * perTask + 0.5));
    }
    if (n > 0 && n < (int)sizeof(buf)) {
        snprintf(buf + n, sizeof(buf) - n,
                 " sum=%.2f | cmds=%u lists=%u | KB/task copy=%.1f swap=%.1f spanw=%u | clk=%.2fus",
                 (double)sum * msPerTick * perTask, (unsigned)(s.cmds * perTask), (unsigned)(s.lists * perTask),
                 (double)s.tmemCopyBytes * perTask / 1024.0, (double)s.tmemSwapBytes * perTask / 1024.0,
                 (unsigned)(s.spanWords * perTask), clk * msPerTick * 1000.0);
    }
    RtLog(buf);
    const double samples = s.triPhSamples > 0 ? (double)s.triPhSamples : 1.0;
    double us[gdxn::TRI_PH_COUNT];
    for (int p = 0; p < gdxn::TRI_PH_COUNT; p++) {
        const double raw = (double)s.triPhTicks[p] - clk * (double)s.triPhN[p];
        us[p] = (raw > 0.0 ? raw : 0.0) * msPerTick * 1000.0 / samples;
    }
    const double top = (double)s.triTopCalls * perTask;
    snprintf(buf, sizeof(buf),
             "[trix] tasks=%u /task calls=%u top=%u rect=%u vrej=%u clip=%u fan=%u allout=%u rej=%u cull=%u "
             "res=%u memo=%u begin=%u emit=%u | us/call screen=%.2f clip=%.2f resolve=%.2f begin=%.2f emit=%.2f "
             "| ms/task screen=%.2f clip=%.2f resolve=%.2f begin=%.2f emit=%.2f (n=%u)",
             s.tasks, (unsigned)(s.triCalls * perTask), (unsigned)top, (unsigned)(s.triRectCalls * perTask),
             (unsigned)(s.triVariantRej * perTask), (unsigned)(s.triClipIn * perTask),
             (unsigned)(s.triClipFan * perTask), (unsigned)(s.triClipAllOut * perTask),
             (unsigned)(s.triTrivialRej * perTask), (unsigned)(s.triCull * perTask),
             (unsigned)(s.triResolveFull * perTask), (unsigned)(s.triResolveMemo * perTask),
             (unsigned)(s.triBegin * perTask), (unsigned)(s.triEmitAll * perTask), us[0], us[1], us[2], us[3],
             us[4], us[0] * top / 1000.0, us[1] * top / 1000.0, us[2] * top / 1000.0, us[3] * top / 1000.0,
             us[4] * top / 1000.0, s.triPhSamples);
    RtLog(buf);
}

GDX_COLD void EmitReceipt() {
    const gdxn::Counters& s = sInterp->Stats();
    char buf[560];
    snprintf(buf, sizeof(buf),
             "[ninterp] tasks=%u lists=%u (wide=%u n64=%u be=%u f3d=%u noop=%u) cmds=%u vtx=%u tri=%u rect=%u "
             "tmem=%u batches=%u unknown=%u dropped=%u runaway=%u ovf=%u rootrej=%u | dl=%u vtxop=%u tri2=%u "
             "settimg=%u ldblk=%u mtx=%u | rej=%u cull=%u emit=%u packref=%u packsplit=%u full=%u "
             "| tex hit=%u miss=%u evict=%u clr=%u inval=%u/%u",
             s.tasks, s.lists, s.listsByClass[0], s.listsByClass[1], s.listsByClass[2], s.listsF3d, s.noopLists,
             s.cmds, s.vtx, s.tris, s.rects, s.tmemLoads, s.batches, s.unknownOps, s.droppedOps, s.runaway,
             s.stackOverflow, s.rootRejects, s.op[0xDE], s.op[0x01], s.op[0x06], s.op[0xFD], s.op[0xF3],
             s.op[0xDA], s.trisRejected, s.trisCulled, s.trisEmitted, s.packRefused, s.packSplit, s.flush[gdxn::FLUSH_FULL],
             s.texHits, s.texMisses, s.texEvicts, s.texClears, s.texInvalidated, s.texInvalidates);
    RtLog(buf);
    /* DROPSITES: the always-on drop receipt. Every one of these counted a silent loss that no
     * log line carried -- the texture-cache decode ceiling in particular had a counter that was
     * printed by nothing at all. It is emitted unconditionally, NOT behind [debug] verbose:
     * putting a drop counter behind a verbose gate is the failure mode itself.
     *   texovf  = decodes above the PICA ceiling (no upload was ever possible)
     *   texgrow = decodes the scratch grew for (pre-fix each of these was a SKIPPED texture
     *             whose cache id kept the previous texture's pixels)
     *   texnoup = imports that decoded nothing and took the neutral fallback
     *   texfb   = neutral fallback uploads issued
     *   texhw   = session high-water decode bytes / current scratch capacity
     *   vtxover = G_VTX loads past the 64-slot window / vertices they refused / high-water end
     *   cliphw  = largest clip polygon the run produced (bounded by 3+7=10) / truncations
     *   texuse  = live cache entries / session high-water / capacity    [B23 cache pressure] */
    const gdxn::Interp::SessionDrops d = sInterp->Drops();
    snprintf(buf, sizeof(buf),
             "[ndrops] texovf=%u texgrow=%u texnoup=%u texfb=%u texhw=%uK/%uK | vtxover=%u/%u hw=%u "
             "| cliphw=%u trunc=%u | texuse=%u/%u/%u | ses tex=%u/%u/%u/%u vtx=%u/%u clip=%u",
             s.texDecodeOverflow, s.texDecodeGrow, s.texNoUpload, s.texFallback,
             sInterp->TexDecodeHwBytes() / 1024u, sInterp->TexScratchCap() / 1024u, s.vtxOver,
             s.vtxOverDropped, sInterp->VtxHwEnd(), sInterp->ClipPolyHw(), s.clipTrunc,
             (unsigned)sInterp->TexLive(), (unsigned)sInterp->TexLiveHw(), (unsigned)gdxn::kTexCacheSize,
             d.texOvf, d.texGrow, d.texNoUp, d.texFb, d.vtxOver, d.vtxDropped, d.clipTrunc);
    RtLog(buf);
    if (sTimgMemo) {
        /* [timgmemo] hit/miss/inert per window, plus the load and skip counts it sits under.
           `inert` = the bridge returned epoch 0 (brfast off, legacy resolve, or a dev gate), so
           the memo stood down and the three original calls ran -- a non-zero inert with the
           lever on is the thing to check before reading any A/B from this run. */
        char tm[320];
        uint32_t ep[8] = {};
        gdx_gfx_timg_class_epoch_parts(ep);
        snprintf(tm, sizeof(tm),
                 "[timgmemo] hit=%u miss=%u (empty=%u epoch=%u key=%u) inert=%u epochchg=%u | loads=%u skips=%u "
                 "unres=%u | rgba16gen=%u venue=%08X ver=%u | chg rgba16=%u venue=%u ver=%u seggen*=%u",
                 s.timgMemoHit, s.timgMemoMiss, s.timgMemoMissEmpty, s.timgMemoMissEpoch, s.timgMemoMissKey,
                 s.timgMemoInert, s.timgEpochChanges, s.tmemLoads, s.tmemSkips, s.timgUnresolved,
                 ep[0], ep[1], ep[2], ep[4], ep[5], ep[6], ep[7]);
        RtLog(tm);
    }
    if (sTmemSkip != 0 || sTmemCls != 0) {
        /* [tmemskip] the widened G_LOADBLOCK/G_LOADTILE same-content skip.
             loads/skips  every TMEM load this window, and the ones that skipped the mirror
             imm/gen      which gate fired: the shipping o2r class, or the staleness test
             desc         loads whose DESCRIPTOR matched the live span -- the population a
                          staleness test is even allowed to look at. desc - imm - gen - stale
                          is the loads the descriptor matched but the LEVEL did not admit.
             stale        descriptor matched and the staleness test said the bytes MOVED. This
                          is the column that carries the invalidation claim: it is the memo
                          catching a real write, not a collision.
             cls          every load by source class: Native / Stable / Dma / Unknown. */
        char ts[320];
        snprintf(ts, sizeof(ts),
                 "[tmemskip] lvl=%d loads=%u skips=%u (imm=%u gen=%u) desc=%u stale=%u | "
                 "cls n=%u s=%u d=%u u=%u | miss span=%u kind=%u words=%u addr=%u other=%u | "
                 "fault=%d inj=%u/%u caught=%u missed=%u",
                 sTmemSkip, s.tmemLoads, s.tmemSkips, s.tmemSkipImm, s.tmemSkipGen, s.tmemDescSame,
                 s.tmemStale, s.tmemClsN, s.tmemClsS, s.tmemClsD, s.tmemClsU, s.tmemMissSpan,
                 s.tmemMissKind, s.tmemMissWords, s.tmemMissAddr, s.tmemMissOther,
                 sTmemFault, s.tmemFaultGen, s.tmemFaultCls, s.tmemFaultCaught, s.tmemFaultMissed);
        RtLog(ts);
    }
    if (&gdx3ds_dynlod_receipt != nullptr) {
        char dl[192];
        if (gdx3ds_dynlod_receipt(dl, (int)sizeof(dl)) > 0) {
            RtLog(dl);
        }
    }
    if (sVerifyOn) {
        snprintf(buf, sizeof(buf),
                 "[nverify] tasks=%u batch_mismatch_tasks=%u hist_mismatch_tasks=%u | cls kind=%u state=%u vbo=%u "
                 "extra=%u missing=%u | field 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u 8=%u 9=%u 10=%u 11=%u",
                 sVerifyChecked, sVerifyMismatches, sHistMismatches, sVfyClsKind, sVfyClsState, sVfyClsVbo,
                 sVfyClsExtra, sVfyClsMissing, sVfyFieldHist[1], sVfyFieldHist[2], sVfyFieldHist[3], sVfyFieldHist[4],
                 sVfyFieldHist[5], sVfyFieldHist[6], sVfyFieldHist[7], sVfyFieldHist[8], sVfyFieldHist[9],
                 sVfyFieldHist[10], sVfyFieldHist[11]);
        RtLog(buf);
        sVerifyDetailWindow = 0;
    }
    {
        char hb[320];
        if (sInterp->HudCensusFormat(hb, sizeof(hb), 64u, 1.0 / CPU_TICKS_PER_MSEC) > 0) {
            RtLog(hb); /* [native] hud_batch census ([hudop], gputrace) */
        }
        char hd[1536];
        if (sInterp->HudDirectFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* [native] hud_direct receipt ([hudd]) */
        }
        if (sInterp->HudElemFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* [native] hudelem receipt */
        }
        if (sInterp->HudRetainFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* [native] hud_retain receipt ([hudr]) */
        }
        if (sInterp->HsxFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* [native] hudsky_emit receipt ([hsx]) */
        }
        if (sInterp->NiMenuLevel() != 0 && sInterp->NiMenuFormat(hd, sizeof(hd), 64u, NiMenuGameMode()) > 0) {
            RtLog(hd); /* nimenu [native] ni_menu receipt ([nimenu]); before [nrrhud] clears the window */
        }
        if (sInterp->NrrHudFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* [native] nrr_hud receipt ([nrrhud]) */
        }
        if (sInterp->DirectFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* C5 Stage 3b direct-load scopes ([dressd]) */
        }
        if (sInterp->DlMemoFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* [native] dlmemo ([dlmemo], c5dlmemo) */
        }
        if (sInterp->DeadSetFormat(hd, sizeof(hd), 64u, 1.0 / CPU_TICKS_PER_MSEC) > 0) {
            RtLog(hd); /* [native] deadsetup ([deadset], c5dlmemo) */
        }
        if (sInterp->NrrSkyFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* [native] nrr_sky ([nrrsky]) */
        }
        if (sInterp->NrrFxFormat(hd, sizeof(hd), 64u) > 0) {
            RtLog(hd); /* [native] nrr_shd / nrr_fx ([nrrfx]) */
        }
        for (int line = 0; line < 16; line++) {
            if (sInterp->SkyFormat(hd, sizeof(hd), line) > 0) {
                RtLog(hd); /* [native] sky_retain / skyclock receipts */
            }
        }
    }
    if (sCensus) {
        EmitCensus(s);
    }
    if (sDspCensus) {
        EmitDspCensus(s);
    }
    EmitDxTrix(s);
    sInterp->ResetStats();
}

/* M0 verify: the bridge converted this job on the submitter; compare its per-opcode input
 * histogram with the owned walk's. Differences name the first opcode that diverged. Known
 * benign classes: the bridge counts the two TEXRECT halves as their own opcodes (so do we),
 * and a G_BRANCH_Z decision differs until [M2] loads vertices. */
void VerifyHistogram(GdxGfxJob* job, const gdxn::Counters& before) {
    unsigned long lists = 0, cmds = 0, ops[256];
    gdx_gfx_job_bridge_stats(job, &lists, &cmds, ops);
    const gdxn::Counters& after = sInterp->Stats();
    sVerifyChecked++;
    /* The bridge's opCounts only count its fast-passthrough opcodes (TRI1/TRI2/QUAD, TEXTURE/
       POPMTX/GEOMETRYMODE, the 0xE2-0xEF RDP state family, 0xF0-0xFC loads/tiles/colours);
       everything else is resolved on its slow path and never counted, so only those compare. */
    int firstOp = -1;
    for (int i = 0; i < 256; i++) {
        const bool passthrough = (i >= 0x05 && i <= 0x07) || (i >= 0xD7 && i <= 0xD9) || (i >= 0xE2 && i <= 0xFC);
        if (!passthrough) continue;
        const unsigned long mine = after.op[i] - before.op[i];
        if (mine != ops[i]) {
            firstOp = i;
            break;
        }
    }
    if (firstOp >= 0) {
        sHistMismatches++;
        if (sHistMismatches <= 8) {
            char buf[240];
            snprintf(buf, sizeof(buf),
                     "[nverify] task=%u first_mismatch op=%02X bridge=%lu owned=%lu | lists bridge=%lu owned=%u "
                     "cmds bridge=%lu owned=%u",
                     after.tasks, firstOp, ops[firstOp], static_cast<unsigned long>(after.op[firstOp] - before.op[firstOp]),
                     lists, after.lists - before.lists, cmds, after.cmds - before.cmds);
            RtLog(buf);
        }
    }
}

} // namespace

/* C5 STAGE 2 [c5cnt] (see C5CntSwitch): format and reset the window's per-category work
 * counters, per RENDERED frame. Called by the [c5cat] emit (gdx3ds_mainprof.c, weak) on its
 * 64-iteration cadence. Returns the length written (0 = nothing counted this window). */
static int C5CntFormatCat(char* buf, int cap, const char* name, const C5CntWin& w, double invR) {
    const double msPerTick = 1.0 / CPU_TICKS_PER_MSEC;
    return snprintf(buf, static_cast<size_t>(cap),
                    " | %s c=%.0f r=%.1f ld=%.1f im=%.1f dc=%.2f b=%.1f d=%.1f tri=%.2f imp=%.2f drw=%.2f", name,
                    static_cast<double>(w.cmds) * invR, static_cast<double>(w.rects) * invR,
                    static_cast<double>(w.loads) * invR, static_cast<double>(w.imports) * invR,
                    static_cast<double>(w.decodes) * invR, static_cast<double>(w.batches) * invR,
                    static_cast<double>(w.draws) * invR, static_cast<double>(w.triTicks) * msPerTick * invR,
                    static_cast<double>(w.impTicks) * msPerTick * invR,
                    static_cast<double>(w.drwTicks) * msPerTick * invR);
}

/* [c5sub] frame= r= | <sub> ms= c= ld= im= d= tri= imp= drw= v= ...: per RENDERED frame; ms is the
 * sub-category's own clock (nested in dress=/fx=), v = marker visits. Only non-empty ones print. */
static void C5SubEmit(unsigned long frame, unsigned rendered) {
    static const char* const kSubName[kC5SubCount] = { "-",      "mine",  "jump",   "ceff",   "pit",   "dirt",
                                                       "dash",   "ice",   "dsign",  "dbldg",  "boost", "expl",
                                                       "debris", "fspark", "cspark", "smoke", "pitff", "fallx" };
    if (rendered == 0) {
        memset(sC5SubWin, 0, sizeof(sC5SubWin));
        return;
    }
    const double invR = 1.0 / static_cast<double>(rendered);
    const double msPerTick = 1.0 / CPU_TICKS_PER_MSEC;
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "[c5sub] frame=%lu r=%u", frame, rendered);
    bool any = false;
    for (int i = 1; i < kC5SubCount && n > 0 && n < static_cast<int>(sizeof(buf)) - 96; i++) {
        const C5SubWin& win = sC5SubWin[i];
        if (win.visits == 0) {
            continue;
        }
        any = true;
        n += snprintf(buf + n, sizeof(buf) - n, " | %s ms=%.3f c=%.0f ld=%.1f im=%.1f d=%.1f tri=%.2f imp=%.2f drw=%.2f v=%.1f",
                      kSubName[i], static_cast<double>(win.ticks) * msPerTick * invR,
                      static_cast<double>(win.w.cmds) * invR, static_cast<double>(win.w.loads) * invR,
                      static_cast<double>(win.w.imports) * invR, static_cast<double>(win.w.draws) * invR,
                      static_cast<double>(win.w.triTicks) * msPerTick * invR,
                      static_cast<double>(win.w.impTicks) * msPerTick * invR,
                      static_cast<double>(win.w.drwTicks) * msPerTick * invR, static_cast<double>(win.visits) * invR);
    }
    if (any) {
        RtLog(buf);
    }
    memset(sC5SubWin, 0, sizeof(sC5SubWin));
}

/* [c5ops] frame= r= hud XX=n ...: the HUD category's walked commands per rendered frame, by opcode. */
static void C5OpsEmit(unsigned long frame, unsigned rendered) {
    if (sC5Ops <= 0) {
        return;
    }
    if (rendered != 0) {
        char buf[1024];
        int n = snprintf(buf, sizeof(buf), "[c5ops] frame=%lu r=%u hud", frame, rendered);
        for (int i = 0; i < 256 && n > 0 && n < static_cast<int>(sizeof(buf)) - 16; i++) {
            if (sC5OpsWin[i] != 0) {
                n += snprintf(buf + n, sizeof(buf) - n, " %02X=%.1f", i,
                              static_cast<double>(sC5OpsWin[i]) / static_cast<double>(rendered));
            }
        }
        RtLog(buf);
    }
    memset(sC5OpsWin, 0, sizeof(sC5OpsWin));
}

extern "C" int gdx3ds_c5cnt_format(char* buf, int cap, unsigned long frame, unsigned rendered) {
    C5SubEmit(frame, rendered);
    static const struct {
        int cat;
        const char* name;
    } kShown[] = { { 9, "hud" }, { 7, "shd" }, { 8, "fx" }, { 5, "mach" } };
    int n = 0;
    if (rendered != 0 && sC5CntWin[9].cmds + sC5CntWin[7].cmds + sC5CntWin[8].cmds != 0) {
        const double invR = 1.0 / static_cast<double>(rendered);
        n = snprintf(buf, static_cast<size_t>(cap), "[c5cnt] frame=%lu r=%u", frame, rendered);
        for (const auto& k : kShown) {
            if (n > 0 && n < cap) {
                n += C5CntFormatCat(buf + n, cap - n, k.name, sC5CntWin[k.cat], invR);
            }
        }
    }
    memset(sC5CntWin, 0, sizeof(sC5CntWin));
    C5OpsEmit(frame, rendered);
    return n < cap ? n : cap - 1;
}

extern "C" void gdxn_logf(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        RtLog(buf);
    }
}

/* The bridge's texture-cache drain broadcasts into the owned cache through these (weakly
 * referenced there, so desktop and the DL harness link nothing). Render thread: the drain is
 * only ever called from GdxGfxJobRun, the VI fallback present, or gdx_ninterp_job_run. */
/* native-all S1 [native] atlas_race_reset (default 0 = the pre-S1 path until hardware S0 clears it; 1 = reset): gdx_rdram_mode_reset
 * asks for it on the mode change into a race, before it queues the clear below; the clear
 * consumes it. Atomic: under the render thread the clear is drained on core 2. */
static std::atomic<int> sAtlasResetAsked{0};
static int sAtlasRaceReset = -1;
extern "C" void gdx_ninterp_atlas_reset_request(void) {
    sAtlasResetAsked.store(1, std::memory_order_release);
}

GDX_COLD static void AtlasRaceResetIfAsked() {
    if (sAtlasResetAsked.exchange(0, std::memory_order_acq_rel) == 0) {
        return;
    }
    if (sAtlasRaceReset < 0) {
        sAtlasRaceReset = gdx3ds_config_get_int("native", "atlas_race_reset", 0) != 0 ? 1 : 0;
        char line[96];
        snprintf(line, sizeof(line), "[atlas] init atlas_race_reset=%d", sAtlasRaceReset);
        RtLog(line);
    }
    if (sAtlasRaceReset != 0) {
        const unsigned retired = sInterp->TexCacheAtlasReset();
        char line[96];
        snprintf(line, sizeof(line), "[atlas] race entry: %u atlas-backed cache entries retired", retired);
        RtLog(line);
    }
}

extern "C" void gdx_ninterp_texcache_clear(void) {
    if (sInterp != nullptr) {
        AtlasRaceResetIfAsked(); /* native-all S1: before the clear, so tex_keep keeps no view */
        sInterp->TexCacheClear();
        sInterp->HudMemoClear(); /* [native] hud_batch: the source/hash memos follow the cache */
        sInterp->HudDirectClear(); /* [native] hud_direct: so does the per-source hash memo */
        sInterp->HudRetainClear(); /* [native] hud_retain: and the retained runs */
        sInterp->SkyClear();       /* [native] sky_retain: the resolved sky textures follow the cache */
        sInterp->NrrHudClear();    /* [native] nrr_hud: state blocks, identities and pending loads */
    }
}

extern "C" void gdx_ninterp_texcache_delete(uintptr_t addr) {
    if (sInterp != nullptr) {
        sInterp->TexCacheDeleteAddress(reinterpret_cast<const uint8_t*>(addr));
    }
}

extern "C" int gdx_ninterp_active(void) {
    Latch();
    return sInterpOn;
}

/* The bridge's prepare skips ConvertRoot when the owned path will walk the job itself. */
extern "C" int gdx_ninterp_owns_walk(void) {
    Latch();
    return (sLatched && sInterpOn && !sVerifyOn) ? 1 : 0;
}

extern "C" int gdx_ninterp_cpuclip(void) {
    Latch();
    return sCpuClip;
}

#ifdef EXPANSION_KIT
/* B11o: read by Interp::DpSetScissor (gdx_rect.cpp), which carries the whole derivation. */
extern "C" int gdx_ninterp_ekwidgetscissor(void) {
    Latch();
    return sEkWidgetScissor;
}

/* [native] ek_editmesh: read by the Interp constructor (gdx_ekpreview.cpp carries the design). */
extern "C" int gdx_ninterp_ekeditmesh(void) {
    Latch();
    return sEkEditMesh;
}
#endif

extern "C" int gdx_ninterp_palhash(void) {
    Latch();
    return sPalHash;
}

/* Read from the bridge (weakly: desktop and the DL harness link neither this TU nor the ini). */
extern "C" int gdx_ninterp_rawcopywhole(void) {
    Latch();
    return sRawCopyWhole;
}

extern "C" int gdx_ninterp_blendflush(void) {
    Latch();
    return sBlendFlush;
}

extern "C" int gdx_ninterp_census(void) {
    Latch();
    return sCensus;
}

extern "C" int gdx_ninterp_ownfold(void) {
    Latch();
    return sOwnFold;
}

/* Census clock. Only called with ncensus=1 (two reads per drained batch). */
extern "C" long long gdx_ninterp_now(void) {
    return gdx3ds_prof_now();
}

/* hwprof [hwsite]: the interpreter's sites (import, hudret, hudrec) reach the timers through these
 * (port/3ds/include/gdx3ds_hwprof.h; weak there, so a link without the module reads "off"). */
extern "C" long long gdx_ninterp_hws_begin(void) {
    return static_cast<long long>(gdx3ds_hws_begin());
}

extern "C" void gdx_ninterp_hws_end(int site, long long t0) {
    gdx3ds_hws_end(site, static_cast<uint64_t>(t0));
}

extern "C" int gdx_ninterp_dspcensus(void) {
    Latch();
    return sDspCensus;
}

/* [native] tmemskip / tmemcls -- see Interp::mTmemSkip. */
extern "C" int gdx_ninterp_tmemskip(void) {
    Latch();
    return sTmemSkip;
}

extern "C" int gdx_ninterp_tmemcls(void) {
    Latch();
    return sTmemCls;
}

/* [memotear] [debug] tmemfault -- see sTmemFault. */
extern "C" int gdx_ninterp_tmemfault(void) {
    Latch();
    return sTmemFault;
}

extern "C" int gdx_ninterp_timgmemo(void) {
    Latch();
    return sTimgMemo;
}

/* o3dsinterp: see sO3dsFast / sTriMemo. */
extern "C" int gdx_ninterp_o3dsfast(void) {
    Latch();
    return sO3dsFast;
}

extern "C" int gdx_ninterp_trimemo(void) {
    Latch();
    return sTriMemo;
}

/* The running [prof] child-section total, so the dispatch census can subtract the vtx/tri/
 * imp/drw/mtx time a handler spent and report SELF time. Read-only: the census must not
 * publish itself as a child, or the RUN ("dsp") bucket it is explaining would go to zero. */
extern "C" unsigned long long gdx_ninterp_child_ticks(void) {
    return (unsigned long long)gdx3ds_prof_child_ticks;
}

/* [debug] nicensus: the game's mode globals, read only (game/gdx3ds_frameskip_game.c; weak so a
 * build without the game half still links). Read on the walking thread at the task's start: with
 * renderthread=0 the game fiber is parked at its VI wait, so the globals are the ones that built
 * the list being walked (gdx3ds_nrr.h, THE LOOK-AHEAD). */
extern "C" int gdx3ds_frameskip_game_race_scope(int* gameMode, int* changeState) __attribute__((weak));
extern "C" int gdx3ds_frameskip_game_racing(void) __attribute__((weak));
extern "C" int gdx3ds_frameskip_game_post_finish(void) __attribute__((weak));
extern "C" int8_t gTitleDemoState __attribute__((weak)); /* decomp game.c: the attract demo (read only) */
/* The NRR core's per-category window counters (nrr/gdx3ds_nrr.cpp, read only). */
extern "C" void gdx3ds_nrr_census_read(uint32_t* markers, uint32_t* draws, uint32_t* verts, int n) __attribute__((weak));

/* nimenu: the game mode for the [nimenu] receipt (read only; -1 without the game half) */
GDX_COLD int NiMenuGameMode() {
    int mode = -1, change = 0;
    if (&gdx3ds_frameskip_game_race_scope != nullptr) {
        (void)gdx3ds_frameskip_game_race_scope(&mode, &change);
    }
    return mode;
}

/* menus-s4: the transition type the game's Transition_Draw drew this frame (decomp transition.c;
 * 0 none, 10 instant). [debug] nicensus_wipe=1 files such frames under the "wipe" sub-scope. */
extern "C" int32_t gGdxXitionDrawnType __attribute__((weak));
static GDX_COLD int NcWipeNow() {
    if (&gGdxXitionDrawnType == nullptr) {
        return 0;
    }
    const int type = static_cast<int>(gGdxXitionDrawnType);
    return (type > 0 && type < 10) ? type : 0;
}

static GDX_COLD int NcBucketNow() {
    if (&gdx3ds_frameskip_game_race_scope == nullptr) {
        return (gdxn::Interp::kNcModes - 1) * gdxn::Interp::kNcSubs;
    }
    int mode = 0, change = 0;
    const int raceScope = gdx3ds_frameskip_game_race_scope(&mode, &change);
    if (mode < 0 || mode >= gdxn::Interp::kNcModes - 1) {
        mode = gdxn::Interp::kNcModes - 1;
    }
    int sub = 0;
    if (&gTitleDemoState != nullptr && gTitleDemoState != 0) {
        sub = 5; /* the attract demo: its races are not the player's */
    } else if (sNicensusWipe != 0 && NcWipeNow() != 0) {
        sub = 6; /* menus-s4: a transition drawn (mode change or not) */
    } else if (change != 0) {
        sub = 4;
    } else if (raceScope != 0) {
        if (gdx3ds_frameskip_game_post_finish()) {
            sub = 3;
        } else if (gdx3ds_frameskip_game_racing()) {
            sub = 2;
        } else {
            sub = 1;
        }
    }
    return mode * gdxn::Interp::kNcSubs + sub;
}

static constexpr int kNcNrrCats = 9;
static uint32_t sNcNrr0[3][kNcNrrCats];

static GDX_COLD void NcNrrRead(uint32_t out[3][kNcNrrCats]) {
    memset(out, 0, sizeof(uint32_t) * 3 * kNcNrrCats);
    if (&gdx3ds_nrr_census_read != nullptr) {
        gdx3ds_nrr_census_read(out[0], out[1], out[2], kNcNrrCats);
    }
}

static GDX_COLD void NcTaskBegin() {
    NcNrrRead(sNcNrr0);
    sInterp->NcFrameBegin(NcBucketNow());
    sInterp->NcFrameWipe(NcWipeNow()); /* menus-s4: the bucket's transition-type histogram */
}

static GDX_COLD void NcTaskEnd() {
    uint32_t now[3][kNcNrrCats];
    NcNrrRead(now);
    for (int k = 0; k < 3; k++) {
        for (int c = 0; c < kNcNrrCats; c++) {
            now[k][c] = now[k][c] >= sNcNrr0[k][c] ? now[k][c] - sNcNrr0[k][c] : now[k][c];
        }
    }
    sInterp->NcFrameEnd(now[0], now[1], now[2], kNcNrrCats);
    sInterp->NcDumpEvery(static_cast<uint32_t>(sNicensusEvery > 0 ? sNicensusEvery : 0), 1.0 / CPU_TICKS_PER_MSEC);
}

static GDX_COLD void NcAtExit() {
    if (sInterp != nullptr) {
        sInterp->NcDump("final", 1.0 / CPU_TICKS_PER_MSEC);
        sInterp->NsDump("final", true); /* [debug] ni_strict (no-op when off) */
    }
}

/* menus-s2 [native] ni_menu_root: a mode whose frame is a MENU region (gdxmenu_capture's set: not an
 * on-track race mode) and not an Expansion Kit editor / the EAD demo (COURSE_EDIT 13, CREATE_MACHINE 16,
 * EAD_DEMO 22: walked as before, their own track). */
static GDX_COLD bool NiMenuRootMode(int mode) {
    return mode >= 0 && mode != 13 && mode != 16 && mode != 22 && mode < gdxn::Interp::kNcModes - 1;
}

/* menus-s4 ni_menu_xition bit 1: a mode-change frame of a non-race mode keeps the menu root programs.
 * The race draw modes (GP 1, practice 2, VS 3-5, time attack 14, death race 21) are not menu frames
 * even while they change (the race scope reads 0 then, so `onTrack` alone cannot tell): gdxmenu_capture's
 * own exclusion set. */
static GDX_COLD bool NiXitionChangeRoot(int mode) {
    if ((sNiMenuXitionMask & 1) == 0) {
        return false;
    }
    return mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 5 && mode != 14 && mode != 21;
}

/* nirace [native] ni_race: the task's race scope (-1 outside it: the programs are disarmed), 0 racing,
 * 1 the rest of a 1P race scope (intro, post-finish); 2 a menu frame under [native] ni_menu_root. Arms
 * the programs for the task. */
static GDX_COLD int NiRaceScope() {
    int scope = -1;
    int multiView = 0;
    if (&gdx3ds_frameskip_game_race_scope != nullptr) {
        int mode = 0, change = 0;
        const int onTrack = gdx3ds_frameskip_game_race_scope(&mode, &change);
        multiView = (mode == 3 || mode == 4 || mode == 5); /* GAMEMODE_VS_2P..VS_4P */
        /* VS: ni_view's race bit (native-vs), or ni_fullview's while the race runs MP fullview (o3ds60) */
        if (onTrack != 0 && change == 0 && (!multiView || sNiRaceVs != 0 || NiRaceFullviewFrame())) {
            scope = (gdx3ds_frameskip_game_racing() && !gdx3ds_frameskip_game_post_finish()) ? 0 : 1;
        } else if (onTrack == 0 && sNiMenuRoot != 0 && NiMenuRootMode(mode) &&
                   (change == 0 || (sNiMenuXition != 0 && NiXitionChangeRoot(mode))) &&
                   (&gTitleDemoState == nullptr || gTitleDemoState == 0)) {
            scope = 2;
            if (change != 0) {
                sXitionWin.changeRoot++; /* menus-s4 */
            }
        }
    }
    if (sNiRaceVs != 0 || sNiRaceFv != 0) {
        /* native-vs: per-view span keys -- race scopes only (0/1); a menu-root frame (2) keeps the ordinal key */
        sInterp->NrrRaceSetMultiView(scope >= 0 && scope != 2 && multiView != 0);
    }
    if (scope == 2) {
        sInterp->NrrRaceArmMenu(true); /* the walk's start runs NrrRaceTaskBegin; no geometry segments */
    } else {
        sInterp->NrrRaceArm(scope >= 0 && sNiRace != 0); /* the walk's start runs NrrRaceTaskBegin */
    }
    return scope;
}

static GDX_COLD void NiRaceTaskEnd(int scope, uint32_t walked) {
    sInterp->NrrRaceArmMenu(false);
    sNiWin.frames[scope]++;
    sNiWin.walked[scope] += walked;
    if (sNiRcpt <= 0 || ++sNiWinFrames < static_cast<uint32_t>(sNiRcpt)) {
        return;
    }
    char buf[512];
    const double r0 = sNiWin.frames[0] != 0 ? static_cast<double>(sNiWin.walked[0]) / sNiWin.frames[0] : 0.0;
    const double r1 = sNiWin.frames[1] != 0 ? static_cast<double>(sNiWin.walked[1]) / sNiWin.frames[1] : 0.0;
    const double r2 = sNiWin.frames[2] != 0 ? static_cast<double>(sNiWin.walked[2]) / sNiWin.frames[2] : 0.0;
    int n = snprintf(buf, sizeof(buf), "[nirace] frames=%u racing=%u walked/f racing=%.1f other=%.1f | ni_race=%d",
                     sNiWinFrames, sNiWin.frames[0], r0, r1, sNiRace);
    if (sNiMenuRoot != 0 && n > 0 && n < static_cast<int>(sizeof(buf))) {
        n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " menu=%u walked/f menu=%.1f", sNiWin.frames[2], r2);
    }
    if ((sNiRace != 0 || sNiMenuRoot != 0) && n > 0 && n < static_cast<int>(sizeof(buf))) {
        sInterp->NrrRaceReceipt(buf + n, static_cast<int>(sizeof(buf)) - n);
    }
    RtLog(buf);
    sNiWin = {};
    sNiWinFrames = 0;
}

/* menus-s4 [xition] receipt, every 64 tasks that drew a transition (none otherwise): the tasks of the
 * window by transition type drawn (t1 small tiles .. t9 wipe), the mode-change frames whose root ran as
 * span programs, and the walker's commands per transition frame. */
static GDX_COLD void XitionTaskEnd(uint32_t walked) {
    const int type = NcWipeNow();
    sXitionWin.tasks++;
    if (type == 0) {
        return;
    }
    sXitionWin.wipe[type]++;
    sXitionWin.walkedWipe += walked;
    uint32_t wipes = 0;
    for (int t = 1; t < 16; t++) {
        wipes += sXitionWin.wipe[t];
    }
    if (wipes < 64) {
        return;
    }
    char buf[400];
    int n = snprintf(buf, sizeof(buf), "[xition] window tasks=%u wipe=%u", sXitionWin.tasks, wipes);
    for (int t = 1; t < 16 && n > 0 && n < static_cast<int>(sizeof(buf)); t++) {
        if (sXitionWin.wipe[t] != 0) {
            n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " t%d=%u", t, sXitionWin.wipe[t]);
        }
    }
    if (n > 0 && n < static_cast<int>(sizeof(buf))) {
        n += snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " | changeroot=%u walked/f=%.1f",
                      sXitionWin.changeRoot, static_cast<double>(sXitionWin.walkedWipe) / wipes);
    }
    RtLog(buf);
    sXitionWin = {};
}

/* Called from gdx_gfx_job_run on the render thread with the job's segment view installed.
 * Returns 1 when the owned path consumed the job. */
extern "C" int gdx_ninterp_job_run(GdxGfxJob* job) {
    Latch();
    if (!sLatched || !sInterpOn || job == nullptr) {
        return 0;
    }
    if (sInterp == nullptr) {
        Fast::GfxRenderingAPIC3D* r = static_cast<Fast::GfxRenderingAPIC3D*>(Gdx3ds_GetCitro3dRenderer());
        sBackend = new BackendC3D(r);
        sInterp = new gdxn::Interp(sBackend);
        sSeam = GdxOwnedSeamCreate(sInterp);
        sInterp->RegisterNoopHook(OwnedNoopHook, sSeam);
        gdx3ds_nrr_mach_bind(sSeam, sInterp); /* [native] nrr_mach: its draw reads P and resets mRs */
        sInterp->RegisterDlHook(OwnedDlHook, sSeam);
        sInterp->SetCiContentHash(sTexHashCi != 0);
        /* DROPSITES levers + forcing knobs (see docs/research/native/dropsites-progress.md). */
        sInterp->SetTexStaleFix(sTexStaleFix != 0);
        sInterp->SetTexKeep(static_cast<uint32_t>(sTexKeepKib > 0 ? sTexKeepKib : 0) * 1024u,
                            static_cast<uint32_t>(sTexKeepFloorKib > 0 ? sTexKeepFloorKib : 0) * 1024u);
        sInterp->SetVtxClamp(sVtxClamp != 0);
        sInterp->SetTexScratchKib(static_cast<uint32_t>(sTexScratchKib), static_cast<uint32_t>(sTexScratchMaxKib));
        sInterp->SetVtxSoftCap(static_cast<uint32_t>(sVtxMax));
        sInterp->SetClipPolyMax(static_cast<uint32_t>(sClipPolyMax));
        gdxn::Interp::TexSkipHooks texHooks = {};
        texHooks.setSlot = gdx3ds_mesh_texlist_slot;
        texHooks.isSpeck = gdx3ds_mesh_is_speck;
        texHooks.isBody = gdx3ds_mesh_is_body;
        texHooks.bodyProbe = TexSkipBodyProbe;
        texHooks.generation = gdx3ds_mesh_generation;
        texHooks.user = sSeam;
        sInterp->SetTexSkip(sTexSkip, texHooks);
        /* [debug] machsplit (c5mach2): diagnostic split of [c5cat] mach; needs the [c5cat] clock. */
        sInterp->MachSplitSet(gdx3ds_config_get_int("debug", "machsplit", 0), gdx3ds_mesh_draw_ticks_total);
        /* [native] machines_loadmemo (c5mach2): the machine set lists' loads, resolution memoised. */
        sInterp->SetLoadMemo(gdx3ds_config_get_int("native", "machines_loadmemo", 0));
        sInterp->HudSetCensus(sHudCensus != 0);
        sInterp->HudSetDirect(sHudDirect);
        sInterp->HudSetClock(gdx3ds_config_get_int("native", "hudclock", 0));
        sInterp->HudSetElem(gdx3ds_config_get_int("native", "hudelem", 0));
        sInterp->HudSetRetain(sHudRetain, gdx3ds_config_get_int("native", "hudretclock", 0));
        sInterp->HsxSetLevel(sHudSkyEmit); /* [native] hudsky_emit (hudsky) */
        NrrHudLatch(); /* [native] nrr_hud (n60hud) */
        sInterp->NrrRaceSet(sNiRace != 0 ? sNiRace : sNiMenuRoot,
                            gdx3ds_config_get_int("debug", "nirace_log", 0)); /* nirace [native] ni_race / ni_menu_root */
        sInterp->NrrRaceRcptSet(sNiRcpt > 0); /* nimenu W4: kNiEnd's cold receipt tally, gated the same as the window */
        if (sNiRaceMesh != 0) {
            sInterp->NrrRaceSetSegHook(NiSegHook, sSeam);
        }
        {
            char nl[256];
            snprintf(nl, sizeof(nl), "[nirace] init ni_race=%d (requested %d) mesh=%d rcpt=%d vs=%d fv=%d menu_root=%d (requested %d)%s%s",
                     sNiRace, sNiRaceReq, sNiRaceMesh, sNiRcpt, sNiRaceVs, sNiRaceFv, sNiMenuRoot, sNiMenuRootReq,
                     (sNiRaceReq != 0 && sVerifyOn) ? " (REFUSED: interp_verify)" : "",
                     (sNiMenuRootReq != 0 && sNiMenuRoot == 0) ? " (menu_root REFUSED: needs ni_menu, not under interp_verify)" : "");
            RtLog(nl);
            snprintf(nl, sizeof(nl),
                     "[xition] init ni_menu_xition=%d (requested %d) mask=0x%X menu_root=%d%s%s", sNiMenuXition,
                     sNiMenuXitionReq, static_cast<unsigned>(sNiMenuXitionMask), sNiMenuRoot,
                     (sNiMenuXitionReq != 0 && sNiMenuXition == 0) ? " (REFUSED: needs ni_menu=1, not under interp_verify)" : "",
                     (sNiMenuXition != 0 && sNiMenuRoot == 0) ? " (change-frame roots inert: needs ni_menu_root=1)" : "");
            RtLog(nl);
        }
        DressLatch();
        sInterp->SkySetLevels(sSkyRetain, sSkyClock, SkyProfRead);
        /* [native] dlmemo / deadsetup (c5dlmemo): not Tier-2, default 0; off under the
           interp_verify oracle, which replays the job through the LUS path as well. */
        sInterp->SetDlMemo(sVerifyOn ? 0 : gdx3ds_config_get_int("native", "dlmemo", 0));
        sInterp->SetDeadSetup(sVerifyOn ? 0 : gdx3ds_config_get_int("native", "deadsetup", 0));
        if (sNicensus != 0) {
            /* [debug] nicensus: measurement only (counters + clocks; nothing drawn changes). Engine only
             * (2) when it is on for [debug] ni_strict alone. */
            sInterp->NcSet(sNicensusDumps != 0 ? 1 : 2);
            sInterp->NsSet(sNiStrict, static_cast<uint32_t>(sNiStrictEvery > 0 ? sNiStrictEvery : 0));
            sInterp->NcTraceSet(gdx3ds_config_get_int("debug", "nicensus_trace", 0),
                                gdx3ds_config_get_int("debug", "nicensus_trace_mode", -1),
                                gdx3ds_config_get_int("debug", "nicensus_trace_skip", 0)); /* menus-s2 */
            if (!sInterp->NcOn()) {
                sNicensus = 0;
                sNicensusDumps = 0;
                sNiStrict = 0;
            } else {
                atexit(NcAtExit);
            }
        }
        {
            char nl[160];
            snprintf(nl, sizeof(nl), "[nicensus] init nicensus=%d every=%d verify=%d", sNicensusDumps, sNicensusEvery,
                     sVerifyOn ? 1 : 0);
            RtLog(nl);
            snprintf(nl, sizeof(nl), "[nistrict] init ni_strict=%d (requested %d) mode=%s every=%d engine=%s",
                     sNiStrict, sNiStrictReq, sNiStrict != 0 ? "count" : "off", sNiStrictEvery,
                     sNiStrict != 0 ? "nicensus" : "-");
            RtLog(nl);
            if (sNiStrictReq == 2) {
                RtLog("[nistrict] ni_strict=2 (drop) is not implemented (TODO S7): counting only, nothing dropped");
            }
        }
    }
    /* The owned cache is the live one under interp=1 and GdxGfxJobRun -- where the LUS path
     * drains -- is never reached, so the drain runs here, at the same point in the task:
     * before the walk, so an eviction raised for this frame's content takes effect now. */
    if (sTexInval) {
        gdx_gfx_texcache_drain();
    }
    GdxFrameConfig cfg = {};
    if (!gdx_gfx_frame_config(&cfg)) {
        if (GDX_UNLIKELY(sNicensus != 0)) {
            sInterp->NcLusTask(NcBucketNow()); /* [debug] nicensus: the LUS path draws this task */
        }
        return 0; /* no window yet: let the LUS path take it (it early-outs the same way) */
    }
    /* Task variant: the job's entry ucode (F3dex2Variant numbering). */
    const int variant = gdx_gfx_job_ucode(job);

    /* [nverify] the pre-task counter snapshot is ~1.2 KB (Counters leads with uint32_t op[256])
     * and is read ONLY by the verify block below and by VerifyHistogram, so it is copied under
     * the lever, not on every task. Audit 2026-09-23 finding 31, same class. */
    if (sVerifyOn) {
        const gdxn::Counters before = sInterp->Stats();
        /* Both paths on one job: LUS records (and draws, epilogue included), the owned path
           replays into the compare ring with the GPU suppressed. */
        static uint32_t sPrevLusEnd[64], sPrevOwnEnd[64];
        static unsigned sPrevN = 0;
        static uint32_t sStartDiffLogs = 0;
        {
            uint32_t lusStart[64], ownStart[64];
            const unsigned n1 = gdx_gfx_lus_state(lusStart, 64);
            const unsigned n2 = sInterp->StateSnapshot(ownStart, 64);
            if (sPrevN != 0 && n1 == sPrevN && n2 == sPrevN && sStartDiffLogs < 40) {
                int lusMoved = -1, ownMoved = -1, diverged = -1;
                for (unsigned i = 0; i < sPrevN; i++) {
                    if (lusMoved < 0 && lusStart[i] != sPrevLusEnd[i]) lusMoved = static_cast<int>(i);
                    if (ownMoved < 0 && ownStart[i] != sPrevOwnEnd[i]) ownMoved = static_cast<int>(i);
                    if (diverged < 0 && lusStart[i] != ownStart[i]) diverged = static_cast<int>(i);
                }
                if (lusMoved >= 0 || ownMoved >= 0 || diverged >= 0) {
                    sStartDiffLogs++;
                    char sb[300];
                    snprintf(sb, sizeof(sb),
                             "[nverify] startstate task=%u lusMoved=%d(%08X->%08X) ownMoved=%d(%08X->%08X) diverged=%d(lus %08X own %08X)",
                             sInterp->Stats().tasks + 1, lusMoved, lusMoved >= 0 ? sPrevLusEnd[lusMoved] : 0,
                             lusMoved >= 0 ? lusStart[lusMoved] : 0, ownMoved, ownMoved >= 0 ? sPrevOwnEnd[ownMoved] : 0,
                             ownMoved >= 0 ? ownStart[ownMoved] : 0, diverged, diverged >= 0 ? lusStart[diverged] : 0,
                             diverged >= 0 ? ownStart[diverged] : 0);
                    RtLog(sb);
                }
            }
        }
        /* One backend, two drivers: each pass re-applies its own cached backend state first so
           its state diff (which assumes the backend still holds what it last sent) stays exact. */
        gdx_gfx_lus_resync_backend();
        gdx3ds_verify_begin(1);
        gdx_gfx_job_run_lus(job);
        uint32_t lusState[64];
        const unsigned lusN = gdx_gfx_lus_state(lusState, 64);
        sInterp->ResyncBackend();
        gdx3ds_verify_begin(2);
        sInterp->RunFrame(job, cfg, variant);
        gdx3ds_verify_begin(0);
        {
            static uint32_t sStateDiffLogs = 0;
            uint32_t ownState[64];
            const unsigned ownN = sInterp->StateSnapshot(ownState, 64);
            memcpy(sPrevLusEnd, lusState, sizeof(sPrevLusEnd));
            memcpy(sPrevOwnEnd, ownState, sizeof(sPrevOwnEnd));
            sPrevN = (lusN == ownN) ? lusN : 0;
            int firstDiff = -1;
            for (unsigned i = 0; i < lusN && i < ownN; i++) {
                if (lusState[i] != ownState[i]) {
                    firstDiff = static_cast<int>(i);
                    break;
                }
            }
            if (firstDiff >= 0 && sStateDiffLogs < 40) {
                sStateDiffLogs++;
                char sb[300];
                snprintf(sb, sizeof(sb), "[nverify] endstate task=%u first_diff_word=%d lus=%08X owned=%08X (w0..3 lus %08X %08X %08X %08X owned %08X %08X %08X %08X)",
                         sInterp->Stats().tasks, firstDiff, lusState[firstDiff], ownState[firstDiff], lusState[0], lusState[1],
                         lusState[2], lusState[3], ownState[0], ownState[1], ownState[2], ownState[3]);
                RtLog(sb);
            }
        }
        unsigned rec = 0, cmp = 0, first = 0, cls = 0, detail = 0;
        gdx3ds_verify_result(&rec, &cmp, &first, &cls, &detail);
        sVerifyChecked++;
        if (first != 0xFFFFFFFFu) {
            sVerifyMismatches++;
            if (cls & 1) sVfyClsKind++;
            if (cls & 2) sVfyClsState++;
            if (cls & 4) sVfyClsVbo++;
            if (cls & 8) sVfyClsExtra++;
            if (cls & 16) sVfyClsMissing++;
            if (detail < 12) sVfyFieldHist[detail]++;
            if (sVerifyDetailWindow < kVerifyDetailPerWindow) {
                sVerifyDetailWindow++;
                char buf[520];
                snprintf(buf, sizeof(buf),
                         "[nverify] task=%u batches lus=%u owned=%u first_mismatch=%u class=%s%s%s%s%s field=%u",
                         sInterp->Stats().tasks, rec, cmp, first, (cls & 1) ? "kind " : "", (cls & 2) ? "state " : "",
                         (cls & 4) ? "vbo " : "", (cls & 8) ? "extra " : "", (cls & 16) ? "missing " : "", detail);
                RtLog(buf);
                gdx3ds_verify_first_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_tri_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_src_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_bytes_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_seq_detail(buf, sizeof(buf));
                RtLog(buf);
                gdx3ds_verify_cmd_detail(buf, sizeof(buf));
                RtLog(buf);
                {
                    unsigned long bl = 0, bc = 0, ops[256];
                    gdx_gfx_job_bridge_stats(job, &bl, &bc, ops);
                    const gdxn::Counters& st = sInterp->Stats();
                    snprintf(buf, sizeof(buf),
                             "[nverify]   walk bridge lists=%lu cmds=%lu | owned lists=%u cmds=%u bz=%u noop=%u drop=%u var=%d",
                             bl, bc, st.lists - before.lists, st.cmds - before.cmds, st.branchZTaken - before.branchZTaken,
                             st.noopLists - before.noopLists, st.droppedOps - before.droppedOps, sInterp->Variant());
                    RtLog(buf);
                }
            }
        }
        VerifyHistogram(job, before);
        if (++sTasksSinceReceipt >= 64) {
            sTasksSinceReceipt = 0;
            EmitReceipt();
        }
        return 1;
    }
    uint64_t profSnap = 0, profT0 = 0;
    const bool profOn = gdx3ds_prof_active != 0;
    if (profOn) {
        profT0 = gdx3ds_prof_enter(&profSnap);
    }
    const int c5Prev = C5CatOpenWalk();
    if (GDX_UNLIKELY(sNicensus != 0)) {
        NcTaskBegin(); /* [debug] nicensus */
    }
    const int niScope = (sNiRace != 0 || sNiRcpt > 0 || sNiMenuRoot != 0) ? NiRaceScope() : -1; /* nirace */
    const uint32_t niCmds0 = sInterp->Stats().cmds;
    sInterp->DirectScope(0); /* C5 Stage 3b: a scope never outlives its task */
    sInterp->RunFrame(job, cfg, variant);
    if (GDX_UNLIKELY(niScope >= 0)) {
        NiRaceTaskEnd(niScope, sInterp->Stats().cmds - niCmds0); /* nirace receipt window */
    }
    if (GDX_UNLIKELY(sNiMenuXition != 0)) {
        XitionTaskEnd(sInterp->Stats().cmds - niCmds0); /* menus-s4 [xition] window */
    }
    sInterp->DirectScope(0);
    sInterp->NrrHudTaskEnd();    /* [native] nrr_hud: the HUD's virtual loads into TMEM, the freshness sequence */
    sInterp->HudDirectTaskEnd(); /* [native] hud_direct: settle the TMEM the HUD still owes */
    sInterp->HudRetainTaskEnd(); /* [native] hudelem / hud_retain: the task's run table */
    sInterp->SkyTaskEnd();       /* [native] sky_retain / skyclock */
#ifdef EXPANSION_KIT
    sInterp->EkPvTaskEnd();    /* [native] ek_editmesh: the preview memo's receipt and idle free */
#endif
    if (GDX_UNLIKELY(sNicensus != 0)) {
        NcTaskEnd(); /* [debug] nicensus (before the NRR window may reset its counters) */
    }
    gdx3ds_nrr_task_end();       /* [native] nrr: receipt cadence (one int test when off) */
    if (profOn) {
        gdx3ds_prof_exit(GDX3DS_PROF_RUN, profT0, profSnap);
    }
    C5CatCloseWalk(c5Prev);
    const int c5Mirr = C5CatEnter(11 /* GDX3DS_C5CAT_MIRR */);
    gdx_gfx_job_epilogue(job);
    C5CatCloseWalk(c5Mirr);
    gdx_perf_sub_end(GDX_PERF_SUB_GFXRUN); /* GdxGfxJobPrepare opened it; GdxGfxJobRun's guard would close it */

    if (++sTasksSinceReceipt >= 64) {
        sTasksSinceReceipt = 0;
        EmitReceipt();
    }
    return 1;
}
