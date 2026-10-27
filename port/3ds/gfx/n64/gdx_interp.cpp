/* port/3ds/gfx/n64/gdx_interp.cpp -- frame driver, per-task reset, receipts, and the
 * handlers that are pure state writes. T&L, TMEM, texcache, combiner and emission live in
 * their own TUs (gdx_rsp / gdx_tmem / gdx_texcache / gdx_cc / gdx_tri) as the milestones land;
 * until then the handlers below that only count are marked [M1]/[M2]. */
#include "gdx_interp.h"
#include "gdx3ds_replay.h" // [native] replay (C6)

#include <math.h>
#include <string.h>

#include "gdx_dlsrc_bridge.h"
#include "gdx_log.h"

namespace gdxn {

/* [native] levers latched by gdx_rt_glue.cpp (weak-free: the host harness supplies stubs). */
extern "C" int gdx_ninterp_census(void);
extern "C" int gdx_ninterp_ownfold(void);
extern "C" int gdx_ninterp_dspcensus(void);
extern "C" int gdx_ninterp_timgmemo(void);
extern "C" int gdx_ninterp_tmemskip(void);
extern "C" int gdx_ninterp_tmemfault(void);
extern "C" int gdx_ninterp_tmemcls(void);
extern "C" int gdx_ninterp_o3dsfast(void);
extern "C" int gdx_ninterp_trimemo(void);
#ifdef EXPANSION_KIT
extern "C" int gdx_ninterp_ekeditmesh(void);
#endif

Interp::Interp(Backend* backend) : mBackend(backend) {
    memset(&mRsp, 0, sizeof(mRsp));
    memset(&mRdp, 0, sizeof(mRdp));
    memset(&mRs, 0, sizeof(mRs));
    memset(&mStats, 0, sizeof(mStats));
    memset(&mCfg, 0, sizeof(mCfg));
    memset(mUnknownLogged, 0, sizeof(mUnknownLogged));
    memset(&mPackParams, 0, sizeof(mPackParams));
    memset(&mPackAux, 0, sizeof(mPackAux));
    memset(mTex, 0, sizeof(mTex));
    for (unsigned i = 0; i < kTexCacheHash; i++) {
        mTexHash[i] = -1;
    }
    mRs.textures[0] = mRs.textures[1] = -1;
    mRdp.viewport_or_scissor_changed = true;
    mRdp.tmem_generation = 1;
    mRsp.modelview_matrix_stack_size = 1;
    mRsp.viewport_z_scale = 511.0f;
    mRsp.viewport_z_trans = 511.0f;
    mRsp.current_num_lights = 2;
    for (int k = 0; k < 256; k++) {
        mByteToUnit[k] = k / 255.0f; /* == (int)b / 255.0f, the LUS table */
    }
    mTexUpload = new uint8_t[kTexUploadBytes];
    mTexUploadCap = kTexUploadBytes; /* DROPSITES: the INITIAL size; UploadPrepare grows it */
    mTrectBatchOn = mBackend->TrectBatchEnabled();
    mCensus = gdx_ninterp_census();
    mOwnFold = gdx_ninterp_ownfold();
    mDspCensus = gdx_ninterp_dspcensus();
    mTimgMemo = gdx_ninterp_timgmemo();
    mTmemSkip = gdx_ninterp_tmemskip();
    mTmemCls = gdx_ninterp_tmemcls();
    /* [memotear] B54 fault injection -- inert unless tmemskip is also on (the injector is only
       ever reached from an admitted skip), and 0 in every shipped configuration. */
    mTmemFault = gdx_ninterp_tmemfault();
    memset(mTimgClassMemo, 0, sizeof(mTimgClassMemo));
    /* o3dsinterp (NEXT.md C4m): the exact rewrites and the tri state memo. */
    mO3dsFast = gdx_ninterp_o3dsfast();
    mTriMemoOn = gdx_ninterp_trimemo();
#ifdef EXPANSION_KIT
    mEkPvOn = gdx_ninterp_ekeditmesh();
#endif
    memset(&mTmKey, 0, sizeof(mTmKey));
    memset(&mTmState, 0, sizeof(mTmState));
}

Interp::~Interp() {
    delete[] mTexRecs; /* [native] machines_texskip */
    DeadSetFree(); /* [native] dlmemo + deadsetup (gdx_dlmemo.cpp) */
    delete[] mTexUpload;
    NcSet(0); /* [debug] nicensus (gdx_nicensus.cpp) */
}

void Interp::ResetStats() {
    /* DROPSITES: roll the drop counters into the session totals before the window is cleared. */
    mSesDrops.texOvf += mStats.texDecodeOverflow;
    mSesDrops.texGrow += mStats.texDecodeGrow;
    mSesDrops.texNoUp += mStats.texNoUpload;
    mSesDrops.texFb += mStats.texFallback;
    mSesDrops.vtxOver += mStats.vtxOver;
    mSesDrops.vtxDropped += mStats.vtxOverDropped;
    mSesDrops.clipTrunc += mStats.clipTrunc;
    memset(&mStats, 0, sizeof(mStats));
}

void Interp::ResetRenderStateCache() {
    mRs.shader_program = nullptr;
    mRs.depth_test_and_mask = 0xFF;
    mRs.decal_mode = 0xFF;
    mRs.alpha_blend = 0xFF;
    memset(&mRs.viewport, 0, sizeof(mRs.viewport));
    memset(&mRs.scissor, 0, sizeof(mRs.scissor));
    mRdp.viewport_or_scissor_changed = true;
    for (int i = 0; i < 2; i++) {
        mRs.textures[i] = -1;
        mRs.sampler_valid[i] = false;
    }
    ArmTexturesChanged();
}

void Interp::ApplyViewportScissor() {
    if (!mRdp.viewport_or_scissor_changed) {
        return;
    }
    if (memcmp(&mRdp.viewport, &mRs.viewport, sizeof(Rect)) != 0) {
        Flush(FLUSH_VIEWPORT);
        mBackend->SetViewport(mRdp.viewport.x, mRdp.viewport.y, mRdp.viewport.width, mRdp.viewport.height);
        mRs.viewport = mRdp.viewport;
    }
    if (memcmp(&mRdp.scissor, &mRs.scissor, sizeof(Rect)) != 0) {
        Flush(FLUSH_SCISSOR);
        mBackend->SetScissor(mRdp.scissor.x, mRdp.scissor.y, mRdp.scissor.width, mRdp.scissor.height);
        mRs.scissor = mRdp.scissor;
    }
    mRdp.viewport_or_scissor_changed = false;
}

void Interp::ResyncBackend() {
    if (mRs.shader_program != nullptr) {
        mBackend->LoadShader(mRs.shader_program);
    }
    if (mRs.depth_test_and_mask != 0xFF) {
        mBackend->SetDepthTestAndMask((mRs.depth_test_and_mask & 1) != 0, (mRs.depth_test_and_mask & 2) != 0);
    }
    if (mRs.decal_mode != 0xFF) {
        mBackend->SetZmodeDecal(mRs.decal_mode != 0);
    }
    if (mRs.alpha_blend != 0xFF) {
        mBackend->SetUseAlpha(mRs.alpha_blend != 0);
    }
    for (int i = 0; i < 2; i++) {
        if (mRs.textures[i] >= 0) {
            mBackend->SelectTexture(i, mTex[mRs.textures[i]].texture_id);
            if (mRs.sampler_valid[i]) {
                mBackend->SetSamplerParameters(i, mRs.sampler_linear[i], mRs.sampler_cms[i], mRs.sampler_cmt[i]);
            }
        }
    }
}

unsigned Interp::StateSnapshot(uint32_t* out, unsigned cap) const {
    if (cap < 48) return 0;
    const Rdp& r = mRdp;
    const Rsp& p = mRsp;
    unsigned n = 0;
    out[n++] = static_cast<uint32_t>(r.combine_mode);
    out[n++] = static_cast<uint32_t>(r.combine_mode >> 32);
    out[n++] = r.other_mode_h;
    out[n++] = r.other_mode_l;
    const auto pack = [](const Rgba& c) { return (static_cast<uint32_t>(c.r) << 24) | (c.g << 16) | (c.b << 8) | c.a; };
    out[n++] = pack(r.prim_color);
    out[n++] = pack(r.env_color);
    out[n++] = pack(r.blend_color);
    out[n++] = pack(r.fill_color);
    out[n++] = pack(r.fog_color);
    out[n++] = (static_cast<uint32_t>(r.prim_lod_fraction) << 16) | r.prim_depth;
    out[n++] = (static_cast<uint32_t>(r.first_tile_index) << 16) | (r.textures_changed[0] ? 1u : 0u) | (r.textures_changed[1] ? 2u : 0u);
    out[n++] = (static_cast<uint32_t>(p.texture_scaling_factor.s) << 16) | p.texture_scaling_factor.t;
    out[n++] = p.geometry_mode;
    out[n++] = (static_cast<uint32_t>(static_cast<uint16_t>(r.viewport.x)) << 16) | static_cast<uint16_t>(r.viewport.y);
    out[n++] = (r.viewport.width << 16) | r.viewport.height;
    out[n++] = (static_cast<uint32_t>(static_cast<uint16_t>(r.scissor.x)) << 16) | static_cast<uint16_t>(r.scissor.y);
    out[n++] = (r.scissor.width << 16) | r.scissor.height;
    out[n++] = (static_cast<uint32_t>(static_cast<int16_t>(p.fog_mul)) << 16) | static_cast<uint16_t>(p.fog_offset);
    out[n++] = p.current_num_lights | (static_cast<uint32_t>(p.modelview_matrix_stack_size) << 8);
    out[n++] = r.color_image_address == r.z_buf_address ? 1u : 0u;
    for (int t = 0; t < 8; t++) {
        const Tile& tt = r.texture_tile[t];
        out[n++] = (static_cast<uint32_t>(tt.fmt) << 28) | (static_cast<uint32_t>(tt.siz) << 24) | (static_cast<uint32_t>(tt.tmem_index) << 8) | tt.palette;
        out[n++] = (tt.line_size_bytes << 16) | (tt.cms << 12) | (tt.cmt << 8) | (tt.masks << 4) | tt.maskt;
        out[n++] = (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int>(tt.uls))) << 16) | static_cast<uint16_t>(static_cast<int>(tt.ult));
        out[n++] = (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int>(tt.lrs))) << 16) | static_cast<uint16_t>(static_cast<int>(tt.lrt));
    }
    return n;
}

bool Interp::UnknownLoggedOnce(uint8_t op) {
    const uint32_t bit = 1u << (op & 31u);
    uint32_t& word = mUnknownLogged[op >> 5];
    if ((word & bit) != 0) {
        return false;
    }
    word |= bit;
    return true;
}

/* == Interpreter::SpReset (interpreter.cpp:9745), minus the LUS-only memos. The lookat
 * coefficient recompute lands with gdx_rsp [M2]; the direction defaults are set here. */
void Interp::SpReset() {
    mRsp.extra_geometry_mode = 0;
    mRsp.modelview_matrix_stack_size = 1;
    mBranchZValid = false;
    mRsp.viewport_z_scale = 511.0f;
    mRsp.viewport_z_trans = 511.0f;
    mRsp.current_num_lights = 2;
    mRsp.lights_changed = true;
    memset(mRsp.dmem, 0, sizeof(mRsp.dmem));
    mRsp.dma_io_dmem = 0;
    mRsp.dma_io_loaded = false;
    mRsp.f3dflx_alpha_light_valid = false;
    mRsp.lookat[0].dir[0] = 0;
    mRsp.lookat[0].dir[1] = 127;
    mRsp.lookat[0].dir[2] = 0;
    mRsp.lookat[1].dir[0] = 127;
    mRsp.lookat[1].dir[1] = 0;
    mRsp.lookat[1].dir[2] = 0;
    CalculateNormalDir(&mRsp.lookat[0], mRsp.current_lookat_coeffs[0]);
    CalculateNormalDir(&mRsp.lookat[1], mRsp.current_lookat_coeffs[1]);
    mLineSection = false;
    mBufNumTris = 0;
    mPackBatch = false;
    mPackDst = nullptr;
    mPackComb = nullptr;
    mPackFolded = false;
}

void Interp::RunFrame(GdxGfxJob* job, const GdxFrameConfig& cfg, int variant) {
    mSrc.Begin(job);
    mCfg = cfg;
    mVariant = variant;
    /* [nverify] latch the mode for the whole task: the glue calls gdx3ds_verify_begin(2)
     * immediately before RunFrame and gdx3ds_verify_begin(0) immediately after, so this is
     * exact, and it keeps the hot sites free of a cross-TU / virtual call. */
    mVerifyActive = mBackend->VerifyActive();
    /* [dspx]/[trix] ride the [prof] gate; dspcensus runs its own (heavier) clocks instead. */
    mDxOn = mBackend->ProfActive() && mDspCensus == 0;
    /* [native] trimemo: a new task starts from a freshly started backend frame and a reset
       mRs.viewport/scissor, so nothing banked by the previous task may be reused. */
    mTmKey.valid = false;
    mStats.tasks++;
    mBatchesAtTaskStart = mStats.batches;
    SpReset();

    /* == Interpreter::Run prologue (interpreter.cpp:10016-10060): the fresh canvas. Run()
     * re-latches the fixed-aspect flag PER TASK and re-derives the render-target decision from
     * it (a mode flip between StartFrame and the task); on this port the viewport always
     * matches and MSAA is 1, so rendersToFb == pillarbox == !widescreen || forceFixedAspect. */
    const bool rendersToFb = (cfg.widescreen == 0) || (cfg.forceFixedAspect != 0);
    mCfg.rendersToFb = rendersToFb ? 1 : 0;
    mBackend->UpdateFramebufferParameters(0, cfg.windowW, cfg.windowH, 1, false, true, true, !rendersToFb);
    mBackend->StartFrame();
    if (gdx3ds_replay_interp != nullptr) {
        /* [native] replay (C6): the walk's starting matrix state, and the verified replay (the
         * backend frame is open, nothing of this task is drawn yet) */
        gdx3ds_replay_interp->taskBegin(&mRsp.P_matrix[0][0], &mRsp.modelview_matrix_stack[0][0][0],
                                        mRsp.modelview_matrix_stack_size, &mRsp.MP_matrix[0][0]);
    }
    mBackend->StartDrawToFramebuffer(rendersToFb ? cfg.gameFb : 0,
                                     static_cast<float>(cfg.curH) / static_cast<float>(cfg.nativeH));
    mBackend->ClearFramebuffer(true, true);
    mRdp.viewport_or_scissor_changed = true;
    memset(&mRs.viewport, 0, sizeof(mRs.viewport));
    memset(&mRs.scissor, 0, sizeof(mRs.scissor));
    /* The port's after-clear hook (boot-logo seed) is env-gated and off on 3DS; not carried. */

    ListRef root;
    if (mSrc.Root(root)) {
        const int ncPrev = (mNc != nullptr) ? NcSwitch(NC_WALK) : -1; /* [debug] nicensus */
        Walk(root);
        if (ncPrev >= 0) {
            NcSwitch(ncPrev);
        }
    } else {
        mStats.rootRejects++;
    }
    Flush(FLUSH_TASK_END);
    TexSkipTaskEnd(); /* [native] machines_texskip: no window may outlive the task */
    if (mDs != nullptr) {
        DeadSetTaskEnd(); /* [native] deadsetup: no window may outlive the task */
    }
    MachSplitTaskEnd(); /* [debug] machsplit receipt cadence */
    LoadMemoTaskEnd();  /* [native] machines_loadmemo receipt cadence */
    if (mCensus) {
        const uint32_t n = mStats.batches - mBatchesAtTaskStart;
        if (n > mStats.maxBatchesPerTask) {
            mStats.maxBatchesPerTask = n;
        }
    }

    /* == Run epilogue: publish the offscreen target when the game renders to it. */
    uintptr_t gfxFrameBuffer = 0;
    if (rendersToFb) {
        mBackend->StartDrawToFramebuffer(0, 1);
        mBackend->ClearFramebuffer(true, true);
        gfxFrameBuffer = reinterpret_cast<uintptr_t>(mBackend->GetFramebufferTextureId(cfg.gameFb));
    }
    gdx_gfx_frame_publish(gfxFrameBuffer);
}

/* ---------------------------------------------------------------------------------------
 * State handlers that are pure writes (byte-for-byte the LUS bodies).
 * --------------------------------------------------------------------------------------- */

void Interp::SpGeometryMode(uint32_t clear, uint32_t set) {
    mRsp.geometry_mode &= ~clear;
    mRsp.geometry_mode |= set;
}

void Interp::SpSetOtherMode(uint32_t shift, uint32_t numBits, uint64_t mode) {
    const uint64_t mask = (((static_cast<uint64_t>(1) << numBits) - 1) << shift);
    uint64_t om = (static_cast<uint64_t>(mRdp.other_mode_h) << 32) | mRdp.other_mode_l;
    om = (om & ~mask) | mode;
    mRdp.other_mode_l = static_cast<uint32_t>(om);
    mRdp.other_mode_h = static_cast<uint32_t>(om >> 32);
}

void Interp::DpSetOtherMode(uint32_t h, uint32_t l) {
    mRdp.other_mode_h = h;
    mRdp.other_mode_l = l;
}

void Interp::SpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    (void)level;
    (void)on;
    mRsp.texture_scaling_factor.s = sc;
    mRsp.texture_scaling_factor.t = tc;
    if (mRdp.first_tile_index != tile) {
        mRdp.textures_changed[0] = true;
        mRdp.textures_changed[1] = true;
    }
    mRdp.first_tile_index = tile;
}

void Interp::SpMoveword(uint8_t index, uint16_t offset, const Cmd& c) {
    switch (index) {
        case MW_NUMLIGHT:
            mRsp.current_num_lights = static_cast<uint8_t>(c.w1 / 24 + 1);
            mRsp.lights_changed = true;
            break;
        case MW_FOG: {
            const int16_t mul = static_cast<int16_t>(c.w1 >> 16);
            const int16_t off = static_cast<int16_t>(c.w1);
            if (mRsp.fog_mul != mul || mRsp.fog_offset != off) {
                Flush(FLUSH_FOG);
            }
            mRsp.fog_mul = mul;
            mRsp.fog_offset = off;
            break;
        }
        case MW_SEGMENT:
            mSrc.SetSegment(offset / 4u, c);
            break;
        default:
            break;
    }
}

void Interp::DpSetCombineMode(uint32_t rgb, uint32_t alpha, uint32_t rgb2, uint32_t alpha2) {
    mRdp.combine_mode = static_cast<uint64_t>(rgb) | (static_cast<uint64_t>(alpha) << 16) |
                        (static_cast<uint64_t>(rgb2) << 28) | (static_cast<uint64_t>(alpha2) << 44);
}

static inline bool RgbaDiffers(const Rgba& a, const Rgba& b) {
    return a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a;
}

/* prim/env/fog split the open batch on a VALUE change only (lus-3ds-primenv-flush): the
 * backend binds them as stage constants from vertex 0 of the flushed batch. */
void Interp::DpSetEnvColor(Rgba c) {
    if (RgbaDiffers(mRdp.env_color, c)) {
        NoteRegChange(false, FLUSH_ENV);
    }
    mRdp.env_color = c;
}
void Interp::DpSetPrimColor(uint8_t m, uint8_t l, Rgba c) {
    (void)m;
    if (RgbaDiffers(mRdp.prim_color, c) || mRdp.prim_lod_fraction != l) {
        NoteRegChange(true, FLUSH_PRIM);
    }
    mRdp.prim_lod_fraction = l;
    mRdp.prim_color = c;
}
void Interp::DpSetFogColor(Rgba c) {
    if (RgbaDiffers(mRdp.fog_color, c)) {
        Flush(FLUSH_FOGCOLOR);
    }
    mRdp.fog_color = c;
}
extern "C" int gdx_ninterp_blendflush(void);
void Interp::DpSetBlendColor(Rgba c) {
    /* [blend-alpha-flush] Flush() samples blend_color.a lazily for the alpha-compare threshold, so
     * buffered geometry must drain before the register changes (only alpha reaches Flush; r/g/b
     * ride the per-vertex fog/blend path). Mirrors the LUS handler; without it a batch drawn under
     * the OLD blend alpha is submitted with the NEW threshold -- the [nverify] field-7 class. */
    if (c.a != mRdp.blend_color.a && gdx_ninterp_blendflush()) {
        Flush(FLUSH_BLENDA);
    }
    mRdp.blend_color = c;
}
void Interp::DpSetFillColor(uint32_t packed) {
    const uint16_t col16 = static_cast<uint16_t>(packed);
    mRdp.fill_color.r = static_cast<uint8_t>(((col16 >> 11) & 0x1F) * 255 / 31);
    mRdp.fill_color.g = static_cast<uint8_t>(((col16 >> 6) & 0x1F) * 255 / 31);
    mRdp.fill_color.b = static_cast<uint8_t>(((col16 >> 1) & 0x1F) * 255 / 31);
    mRdp.fill_color.a = static_cast<uint8_t>((col16 & 1) * 255);
}
void Interp::DpSetPrimDepth(uint16_t z) {
    /* [prim-depth-flush] Flush samples prim_depth at drain time: drain before the register
     * moves (gfx_set_prim_depth_handler_rdp). */
    if (z != mRdp.prim_depth) {
        Flush(FLUSH_PRIMDEPTH);
    }
    mRdp.prim_depth = z;
}

void Interp::DpSetTextureImage(uint32_t fmt, uint32_t siz, uint32_t width, const Cmd& c) {
    TexSkipNoteWriter(0xFD); /* [native] machines_texskip tripwire */
    /* Resolution is lazy: the load extent is only known at LOADBLOCK/LOADTILE/LOADTLUT time
     * (plan §3.2, replaces the bridge's forward scan + persistent copy). */
    mRdp.texture_to_load.fmt = static_cast<uint8_t>(fmt);
    mRdp.texture_to_load.siz = static_cast<uint8_t>(siz);
    mRdp.texture_to_load.width = width;
    mRdp.texture_to_load.cmd = c;
    mRdp.texture_to_load.addr = nullptr;
    mRdp.texture_to_load.immutable = 0;
    mRdp.texture_to_load.native_rgba16 = 0;
}

void Interp::DpSetColorImage(uint32_t fmt, uint32_t siz, uint32_t width, const Cmd& c) {
    (void)fmt;
    (void)siz;
    (void)width;
    const uintptr_t p = mSrc.ResolveData(c, 1);
    mRdp.color_image_address = p != 0 ? p : (0x40000000u ^ c.w1);
}

void Interp::DpSetZImage(const Cmd& c) {
    const uintptr_t p = mSrc.ResolveData(c, 1);
    mRdp.z_buf_address = p != 0 ? p : (0x40000000u ^ c.w1);
}

} // namespace gdxn
