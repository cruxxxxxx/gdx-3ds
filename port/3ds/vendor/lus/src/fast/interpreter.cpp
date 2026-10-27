#define NOMINMAX

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>

#include <any>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <list>
#include <stack>
#include "fast/resource/type/Light.h"

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "fast/debug/GfxDebugger.h"
#include "fast/types.h"
#include <string>

#include "fast/interpreter.h"
#include "fast/lus_gbi.h"
#include "fast/backends/gfx_window_manager_api.h"
#include "fast/backends/gfx_rendering_api.h"

#include "ship/window/gui/Gui.h"
#include "ship/resource/ResourceManager.h"
#include "ship/utils/Utils.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"

#include "libultraship/libultra/os.h"
#include "libultraship/bridge/consolevariablebridge.h"

#include <spdlog/fmt/fmt.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __3DS__
// [triloop] Direct PICA-layout vertex emission: pack-parameter / batch-aux
// contract with the citro3d backend (port/3ds/gfx/gdx3ds_vbopack.h; the port's
// gdx3ds_lus_carve target adds that directory to this TU's include path).
// Desktop builds never see this include or the packed path.
#include "gdx3ds_vbopack.h"
#include "gdx3ds_track_interp.h" // [native] track marker replay contract
// [native] track: port-defined marker entry (gdx3ds_track_replay.cpp). Weak so a link
// without the port's replay TU (nothing references it) degrades the marker to a no-op.
extern "C" void gdx3ds_track_marker(Fast::Interpreter* interp, uint32_t w0, uintptr_t w1) __attribute__((weak));
extern "C" void gdx3ds_fx_marker(Fast::Interpreter* interp, uint32_t w0, uintptr_t w1) __attribute__((weak));
#include "gdx3ds_verify.h"
#endif

std::stack<std::string> currentDir;

// Run-log shim from the port (port_log.h), used by the env-gated diagnostics below.
extern "C" void gdx_dbg_logf(const char* fmt, ...);

/* C4c (port/3ds/gdx3ds_texupload.c): the decoded-texture staging reservation and the
   extent clamps every decoder below is bounded by. Declared here rather than pulled in as
   a header so the vendored carve keeps exactly one new dependency edge. */
extern "C" size_t gdx3ds_tex_upload_pick_size(size_t defaultBytes);
extern "C" uint32_t gdx3ds_tex_upload_clamp_rows(uint32_t width, uint32_t height);
extern "C" size_t gdx3ds_tex_upload_clamp_bytes(size_t want);

#define SEG_ADDR(seg, addr) (addr | (seg << 24) | 1)
#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_)*0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_)*0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_)*0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

// Based off the current set native dimensions or active framebuffer
#define HALF_SCREEN_WIDTH(activeFb) ((mFbActive ? activeFb->second.orig_width : mNativeDimensions.width) / 2)
#define HALF_SCREEN_HEIGHT(activeFb) ((mFbActive ? activeFb->second.orig_height : mNativeDimensions.height) / 2)

// Ratios for current window dimensions or active framebuffer scaled size
#define RATIO_X(activeFb, dims) \
    ((mFbActive ? activeFb->second.applied_width : dims.width) / (2.0f * HALF_SCREEN_WIDTH(activeFb)))
#define RATIO_Y(activeFb, dims) \
    ((mFbActive ? activeFb->second.applied_height : dims.height) / (2.0f * HALF_SCREEN_HEIGHT(activeFb)))

#define TEXTURE_CACHE_MAX_SIZE 1024

namespace Fast {

static UcodeHandlers ucode_handler_index = ucode_f3dex2;

const static uint32_t f3dex2AttrHandler[] = {
    F3DEX2_G_MTX_PROJECTION, F3DEX2_G_MTX_LOAD,  F3DEX2_G_MTX_PUSH,  F3DEX_G_MTX_NOPUSH,
    F3DEX2_G_CULL_FRONT,     F3DEX2_G_CULL_BACK, F3DEX2_G_CULL_BOTH,
};

const static uint32_t f3dexAttrHandler[] = { F3DEX_G_MTX_PROJECTION, F3DEX_G_MTX_LOAD,   F3DEX_G_MTX_PUSH,
                                             F3DEX_G_MTX_NOPUSH,     F3DEX_G_CULL_FRONT, F3DEX_G_CULL_BACK,
                                             F3DEX_G_CULL_BOTH };

static constexpr std::array ucode_attr_handlers = {
    &f3dexAttrHandler,  // ucode_f3db
    &f3dexAttrHandler,  // ucode_f3d
    &f3dexAttrHandler,  // ucode_f3dex
    &f3dexAttrHandler,  // ucode_f3exb
    &f3dex2AttrHandler, // ucode_f3ex2
    &f3dex2AttrHandler, // ucode_s2dex
};

static uint32_t get_attr(Attribute attr) {
    const auto ucode_map = ucode_attr_handlers[ucode_handler_index];
    // assert(ucode_map->contains(attr) && "Attribute not found in the current ucode handler");
    return (*ucode_map)[attr];
}

static std::string GetPathWithoutFileName(char* filePath) {
    size_t len = strlen(filePath);

    for (size_t i = len - 1; (long)i >= 0; i--) {
        if (filePath[i] == '/' || filePath[i] == '\\') {
            return std::string(filePath).substr(0, i);
        }
    }

    return filePath;
}

constexpr size_t MAX_TRI_BUFFER = 256;

Interpreter::Interpreter() {
    mRsp = new RSP();
    mRdp = new RDP();
    mBufVbo = new float[MAX_TRI_BUFFER * (32 * 3)];
}

Interpreter::~Interpreter() {
    delete mRsp;
    delete mRdp;
    delete[] mBufVbo;
}

static std::weak_ptr<Interpreter> mInstance;
// [perf-s7] Raw mirror of mInstance for the per-command opcode handlers. Every handler used to
// re-lock the weak_ptr (an atomic inc/dec pair on the control block) once per display-list
// command — ~1.2-3.2k commands per race frame of pure refcount traffic. The handlers only run
// inside the interpreter's own DL walk, where the instance is alive by construction, so a raw
// pointer is safe there. Cross-thread readers (gdx_get_widescreen_geometry_xscale et al.) keep
// the checked weak_ptr path.
static Interpreter* sGfxInstanceRaw = nullptr;
// [perf-s7] Compiled-in S7 interpreter optimizations, surfaced to the port's one-shot
// [perf-s7] log line (weak-referenced there so unpatched trees still link).
// bit0 = raw-instance dispatch, bit1 = gated geometry diagnostics, bit2 = tri draw-state memo,
// bit3 = rect-run draw-state memo ([texrect]), bit4 = [triloop] packed vertex emission
// (3DS builds only — the packed path is __3DS__-gated, but the bit advertises the
// compiled-in capability like the others).
extern "C" const unsigned int gdx_s7_lus_optmask = 0x1Fu;

// [perf-s7] Per-triangle draw-state memo. GfxSpTri1 re-derived the texture-unit extents
// (several integer divides per unit), re-queried the backend for ShaderGetInfo and
// GetClipParameters (two virtual calls), and re-walked the sampler guards for EVERY triangle,
// even though consecutive triangles overwhelmingly share identical RDP/RSP draw state (a course
// chunk is one material followed by a long VTX/TRI run). gfx_step marks the state dirty on any
// opcode that can touch draw state (conservative whitelist of pure geometry/flow commands,
// F3DEX2 family only); while clean, GfxSpTri1 reuses the memo below instead of re-deriving.
// Invalidation: any non-whitelisted opcode, SpReset (per task), texture/shader cache clears.
struct GfxS7TriStateMemo {
    bool valid = false;
    const void* comb = nullptr; // ColorCombiner the tile-extent block was derived under
    uint32_t tm = 0;
    uint32_t tex_width[2] = {};
    uint32_t tex_height[2] = {};
    uint32_t tex_width2[2] = {};
    uint32_t tex_height2[2] = {};
    uint32_t effective_tile[2] = {};
    const void* prg = nullptr; // ShaderProgram the cached info below belongs to
    uint8_t numInputs = 0;
    bool usedTextures[2] = {};
    GfxClipParameters clip_parameters = {};
    // [texrect] key for the rect-run memo only: GfxDpTextureRectangle/GfxDpImageRectangle swap
    // mRdp->first_tile_index per rect, so a banked rect derivation is only reusable for rects
    // addressing the SAME tile descriptor. Unused (always banked, never compared) for tris.
    uint8_t first_tile = 0;
};
static GfxS7TriStateMemo sGfxS7TriMemo;

// [texrect] Second memo instance for the rect path (G_TEXRECT/G_FILLRECT via GfxDrawRectangle).
// The tri memo deliberately excludes rect draws: GfxDrawRectangle wraps its GfxSpTri1 calls in
// temporary othermode/viewport/geometry mutations it restores afterwards, so banking THOSE into
// the tri memo would leave a stale memo marked clean for the next game triangle. Rect draws
// instead bank here: every rect runs under the SAME deterministic mutation scope (geometry_mode
// = 0, the fixed rect-bypass viewport, a COPY-mode textfilt/combiner override that is a pure
// function of the surrounding othermode state), so a memo banked by one rect is valid for the
// next rect exactly while no non-whitelisted opcode ran in between (kGfxS7DirtyRect below).
static GfxS7TriStateMemo sGfxS7RectMemo;

// [texrect] Draw-state dirty tracking, one bit per memo consumer. Bit semantics: SET = the
// corresponding memo may no longer describe live state. gfx_step ORs a precomputed per-opcode
// mask in (branchless — replaces the old single-bool branch); the GfxSpTri1 slow path clears
// exactly its own bit when it re-banks.
enum : uint8_t {
    kGfxS7DirtyTri = 1u << 0,  // [perf-s7] tri draw-state memo (whitelist: pure geometry/flow)
    kGfxS7DirtyRect = 1u << 1, // [texrect] rect-run memo (tri whitelist minus tri-emitting ops,
                               // plus the texrect opcodes themselves)
    // [texrect] GfxDrawRectangle's rect-bypass viewport, post-AdjustVIewportOrScissor. A pure
    // function of frame/framebuffer state (mFbActive/mActiveFrameBuffer geometry, native/current
    // dimensions, window viewport, backend clip parameters): every mutation site is either a
    // non-whitelisted opcode (framebuffer set/reset handlers) or a task/frame boundary
    // (SpReset/StartFrame full invalidation), so while this bit is clear the cached value equals
    // what the per-rect derivation — including a virtual GetClipParameters round trip — returns.
    kGfxS7DirtyRectViewport = 1u << 2,
};
static uint8_t sGfxS7DirtyMask = 0xFFu;
static XYWidthHeight sGfxRectAdjustedViewport = {};

static inline void GfxS7InvalidateTriMemo() {
    sGfxS7TriMemo.valid = false;
    sGfxS7DirtyMask |= kGfxS7DirtyTri;
}

#ifdef __3DS__
// [trect] Texrect-RUN census (LOCKED-60 Task A, docs/research/locked60-campaign.md). Every
// G_TEXRECT is classified against the previous rect of the same RUN (a run = rects with no
// game triangle and no task/frame boundary between them): identical draw state, a
// texture-only delta (HUD atlas candidate: same combiner/othermode/prim-env/scissor, only the
// tile image differs), or another delta (first differing group recorded). It also records
// whether the rect cost a BATCH BREAK (a Flush with a non-empty VBO between the previous
// rect's end and this rect's end, split into "between rects" and "inside the rect"), and the
// per-rect time split: ImportTexture (prof IMP section delta), the DrawTriangles submit inside
// Flush, and the remainder (2x GfxSpTri1 tri path + handler). Pure diagnostic: active only
// under the gputrace ([prof] window) or the bridge-mirrored verbose gate, drained by
// gdx_trect_census_format on the [prof]/[race-dl] cadence. The only always-on cost is two
// counter increments (Flush, non-rect GfxSpTri1).
extern "C" int gdx3ds_prof_active;
extern "C" long long gdx3ds_prof_now(void);
extern "C" uint64_t gdx3ds_prof_sec_ticks[7];
extern "C" uint32_t gdx3ds_prof_sec_calls[7];
extern "C" int gdx_trect_census_on = 0; // verbose-gate mirror, written by the bridge
extern "C" void gdx_trect_census_dump(const char* line) __attribute__((weak)); // port sink (filelog)
// [trectbatch] LOCKED-60 Task A lever: HUD atlas + same-page rect batch merge. The census
// proved same-state rects already share a batch and that EVERY texture switch inside a HUD
// run (18/frame steady, 113-138/frame on the race-start crowd and rankings frames) costs a
// batch break = one DrawTriangles + one import lookup (~38 us here). The backend
// (gfx_citro3d.cpp) packs small clamp-addressed rect textures into shared atlas PAGES as
// "view" ids with a per-view UV affine; two views of the same page are the same GPU texture,
// so a rect whose import resolves to a view on the currently bound page keeps the open
// packed batch (the pack params are refreshed for the new view's offset). Contract is
// weak-linked: a port without it keeps the lever off. Killswitch [debug] trectbatch=0 ->
// byte-identical legacy order (flush-before-import, no arming).
extern "C" int gdx3ds_trectbatch_enabled(void) __attribute__((weak));
extern "C" void gdx3ds_atlas_arm(int on) __attribute__((weak));
extern "C" int gdx3ds_tex_page(uint32_t texId) __attribute__((weak));
extern "C" void gdx3ds_vbopack_refresh(Gdx3dsVboPackParams* params) __attribute__((weak));
/* [nverify] The TMEM bytes a load actually occupies. size_bytes counts SOURCE bytes; LOADTILE
 * writes each row at an 8-byte-aligned destination stride, so for a 74-byte row the footprint is
 * 80 per row and the tile's last rows sit BEYOND size_bytes -- which is where the decode reads,
 * and where a hash over size_bytes could not see a divergence. Must match gdx_tri.cpp's
 * VfyTmemFootprint. */
static inline unsigned GdxVfyTmemFootprint(unsigned sizeBytes, unsigned lineBytes) {
    if (lineBytes == 0 || sizeBytes == 0) return sizeBytes;
    const unsigned stride = (lineBytes + 7u) & ~7u;
    if (stride == lineBytes) return sizeBytes;
    const unsigned rows = sizeBytes / lineBytes;
    if (rows == 0) return sizeBytes;
    return (rows - 1u) * stride + lineBytes;
}

/* [native] palhash, owned by the interpreter glue; weak so desktop links without it. */
extern "C" int gdx_ninterp_palhash(void) __attribute__((weak));
extern "C" void gdx3ds_atlas_stats(unsigned long out[5]) __attribute__((weak));
namespace {
bool sTrectBatchOn = false;
bool sTrectRectAtlasOk = false; // this rect's tile is clamp-addressed, unmirrored, UVs inside
uint32_t sTrectBatchMerged = 0;   // same-page view re-import kept the batch open
uint32_t sTrectBatchSwitched = 0; // page changed: batch drawn with its own texture, then split
uint32_t sTrectBatchBypassed = 0; // guard refused the merge attempt (legacy order taken)
uint32_t sTrectBatchArmed = 0;    // eligible rect imports that armed the atlas
uint32_t sTrectViewTriDraws = 0;  // diagnostic: NON-rect imports that resolved to a view
inline void GdxTrectBatchLatch() {
    sTrectBatchOn = &gdx3ds_trectbatch_enabled != nullptr && &gdx3ds_atlas_arm != nullptr &&
                    &gdx3ds_tex_page != nullptr && &gdx3ds_vbopack_refresh != nullptr &&
                    gdx3ds_trectbatch_enabled() != 0;
}
// Atlas eligibility of a rect's tile: clamp (or mask 0 = implicit clamp) on both axes, no
// mirror, and the rect's texel window inside [uls, lrs+1] x [ult, lrt+1] (the gutter then
// reproduces N64 clamp sampling exactly).
inline bool GdxTrectRectEligible(const RDP* rdp, uint8_t tile, int16_t uls, int16_t ult, float lrs, float lrt) {
    const auto& tt = rdp->texture_tile[tile];
    const bool clampS = ((tt.cms & G_TX_CLAMP) != 0 || tt.masks == 0) && !((tt.cms & G_TX_MIRROR) != 0 && tt.masks != 0);
    const bool clampT = ((tt.cmt & G_TX_CLAMP) != 0 || tt.maskt == 0) && !((tt.cmt & G_TX_MIRROR) != 0 && tt.maskt != 0);
    if (!clampS || !clampT) {
        return false;
    }
    // A shifted tile pre-scales its texel window by 2^-shift on the vertex path; keep such
    // tiles standalone rather than reason about the post-shift window here.
    if (tt.shifts != 0 || tt.shiftt != 0) {
        return false;
    }
    const float s0 = (float)uls / 32.0f, s1 = lrs / 32.0f;
    const float t0 = (float)ult / 32.0f, t1 = lrt / 32.0f;
    const float sMin = s0 < s1 ? s0 : s1, sMax = s0 < s1 ? s1 : s0;
    const float tMin = t0 < t1 ? t0 : t1, tMax = t0 < t1 ? t1 : t0;
    // texture_tile uls/lrs are 10.2 fixed point (see the /4.0f in the packed UV write)
    return sMin >= tt.uls / 4.0f - 0.01f && sMax <= tt.lrs / 4.0f + 1.01f && tMin >= tt.ult / 4.0f - 0.01f &&
           tMax <= tt.lrt / 4.0f + 1.01f;
}
} // namespace
namespace {
enum { kTrectProfImp = 4 }; // kGdxProfIMP (declared later in this TU)
struct GdxTrectKey {
    const uint8_t* texAddr;
    uint32_t texSize, lineSize;
    float uls, ult, lrs, lrt;
    uint64_t comb;
    uint32_t omh, oml;
    uint32_t prim, env;
    uint32_t extraGeo;
    XYWidthHeight scissor;
    uint16_t primDepth;
    uint8_t blendA;
    uint8_t fmt, siz, cms, cmt, masks, maskt, palIdx, tile;
};
enum { kTrectClsIdent = 0, kTrectClsTexOnly = 1, kTrectClsOther = 2 };
enum { kTrectCauseOm = 0, kTrectCauseComb, kTrectCausePrimEnv, kTrectCauseScissor, kTrectCauseTile, kTrectCauseN };
struct GdxTrectCensus {
    uint32_t rects, runs;
    uint32_t ident, identFlushed;
    uint32_t texOnly, texOnlyFlushed, texOnlyClamp, texOnlyUvIn;
    uint32_t other, otherFlushed, cause[kTrectCauseN];
    uint32_t flushIn, flushBetween, impCalls;
    uint32_t runHist[5], maxRunLen, maxRunDistinct;
    uint64_t rectTicks, impTicks, flushTicks;
};
GdxTrectCensus sTrect = {};
GdxTrectKey sTrectPrev = {};
bool sTrectPrevValid = false;
uint32_t sTrectFlushSeq = 0;          // Flush() calls that submitted a non-empty VBO
uint32_t sTrectFlushSeqAtPrevEnd = 0;
uint32_t sTrectFlushSeqAtEnter = 0;
uint32_t sTrectTriSeq = 0;            // non-rect GfxSpTri1 calls (run breaker)
uint32_t sTrectTriSeqAtPrevEnd = 0;
bool sTrectInRect = false;
// pending classification of the rect in flight, resolved at exit
bool sTrectPendSameRun = false, sTrectPendFlushedBetween = false, sTrectPendClamp = false, sTrectPendUvIn = false;
int sTrectPendCls = 0, sTrectPendCause = 0;
// per-run / per-window distinct texture tracking (address identity; sizes summed once)
uint32_t sTrectRunLen = 0, sTrectRunDistinct = 0;
const uint8_t* sTrectRunTex[32];
enum { kTrectWinTexCap = 256 };
const uint8_t* sTrectWinTex[kTrectWinTexCap];
uint32_t sTrectWinDistinct = 0, sTrectWinBytes = 0;
// [trect-tex] inventory of the window's distinct rect textures (one-shot dumps, see the drain)
struct GdxTrectTexInfo {
    uint32_t bytes;
    uint16_t w, h;
    uint8_t fmt, siz, cms, cmt, filt, palIdx;
    char tail[20];
};
GdxTrectTexInfo sTrectWinInfo[kTrectWinTexCap];
uint32_t sTrectDumpsLeft = 4;

inline bool GdxTrectActive() {
    return gdx3ds_prof_active != 0 || gdx_trect_census_on != 0;
}

void GdxTrectRunEnd() {
    if (sTrectRunLen == 0) {
        return;
    }
    const uint32_t d = sTrectRunDistinct;
    sTrect.runHist[d <= 1 ? 0 : d <= 4 ? 1 : d <= 8 ? 2 : d <= 16 ? 3 : 4]++;
    if (sTrectRunLen > sTrect.maxRunLen) {
        sTrect.maxRunLen = sTrectRunLen;
    }
    if (d > sTrect.maxRunDistinct) {
        sTrect.maxRunDistinct = d;
    }
    sTrectRunLen = 0;
    sTrectRunDistinct = 0;
}

void GdxTrectNoteTexture(const uint8_t* addr, uint32_t bytes, const GdxTrectTexInfo* info) {
    bool seen = false;
    for (uint32_t i = 0; i < sTrectRunDistinct && i < 32; i++) {
        if (sTrectRunTex[i] == addr) {
            seen = true;
            break;
        }
    }
    if (!seen) {
        if (sTrectRunDistinct < 32) {
            sTrectRunTex[sTrectRunDistinct] = addr;
        }
        sTrectRunDistinct++;
    }
    seen = false;
    for (uint32_t i = 0; i < sTrectWinDistinct && i < kTrectWinTexCap; i++) {
        if (sTrectWinTex[i] == addr) {
            seen = true;
            break;
        }
    }
    if (!seen) {
        if (sTrectWinDistinct < kTrectWinTexCap) {
            sTrectWinTex[sTrectWinDistinct] = addr;
            sTrectWinInfo[sTrectWinDistinct] = *info;
        }
        sTrectWinDistinct++;
        sTrectWinBytes += bytes;
    }
}

// Which group differs first between two keys; -1 = identical.
int GdxTrectDiffGroup(const GdxTrectKey& a, const GdxTrectKey& b, bool* texDiffers) {
    *texDiffers = a.texAddr != b.texAddr || a.texSize != b.texSize || a.lineSize != b.lineSize ||
                  a.fmt != b.fmt || a.siz != b.siz || a.palIdx != b.palIdx || a.uls != b.uls ||
                  a.ult != b.ult || a.lrs != b.lrs || a.lrt != b.lrt;
    if (a.omh != b.omh || a.oml != b.oml) {
        return kTrectCauseOm;
    }
    if (a.comb != b.comb) {
        return kTrectCauseComb;
    }
    if (a.prim != b.prim || a.env != b.env || a.blendA != b.blendA || a.primDepth != b.primDepth) {
        return kTrectCausePrimEnv;
    }
    if (memcmp(&a.scissor, &b.scissor, sizeof(a.scissor)) != 0 || a.extraGeo != b.extraGeo) {
        return kTrectCauseScissor;
    }
    if (a.cms != b.cms || a.cmt != b.cmt || a.masks != b.masks || a.maskt != b.maskt || a.tile != b.tile) {
        return kTrectCauseTile;
    }
    return -1;
}

inline uint32_t GdxTrectPackRgba(const RGBA& c) {
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | (uint32_t)c.a;
}

void GdxTrectEnter(RDP* rdp, const RSP* rsp, uint8_t tile, int16_t uls, int16_t ult, float lrs, float lrt,
                   uint64_t comb) {
    const auto& tt = rdp->texture_tile[tile];
    const LoadedTexture& lt = rdp->LoadedTextureAt(tt.tmem_index);
    GdxTrectKey k;
    memset(&k, 0, sizeof(k));
    k.texAddr = lt.addr;
    k.texSize = lt.size_bytes;
    k.lineSize = tt.line_size_bytes;
    k.uls = tt.uls;
    k.ult = tt.ult;
    k.lrs = tt.lrs;
    k.lrt = tt.lrt;
    k.comb = comb;
    k.omh = rdp->other_mode_h;
    k.oml = rdp->other_mode_l;
    k.prim = GdxTrectPackRgba(rdp->prim_color);
    k.env = GdxTrectPackRgba(rdp->env_color);
    k.extraGeo = rsp->extra_geometry_mode;
    k.scissor = rdp->scissor;
    k.primDepth = rdp->prim_depth;
    k.blendA = rdp->blend_color.a;
    k.fmt = tt.fmt;
    k.siz = tt.siz;
    k.cms = tt.cms;
    k.cmt = tt.cmt;
    k.masks = tt.masks;
    k.maskt = tt.maskt;
    k.palIdx = tt.palette;
    k.tile = tile;

    const bool sameRun = sTrectPrevValid && sTrectTriSeq == sTrectTriSeqAtPrevEnd;
    if (!sameRun) {
        GdxTrectRunEnd();
        sTrect.runs++;
    }
    sTrect.rects++;
    sTrectRunLen++;
    {
        GdxTrectTexInfo info;
        memset(&info, 0, sizeof(info));
        info.bytes = lt.size_bytes;
        info.w = (uint16_t)((tt.lrs - tt.uls + 4.0f) / 4.0f); // 10.2 fixed -> texels
        info.h = (uint16_t)((tt.lrt - tt.ult + 4.0f) / 4.0f);
        info.fmt = tt.fmt;
        info.siz = tt.siz;
        info.cms = tt.cms;
        info.cmt = tt.cmt;
        info.filt = (uint8_t)((rdp->other_mode_h >> G_MDSFT_TEXTFILT) & 3u);
        info.palIdx = tt.palette;
        auto* res = lt.raw_tex_metadata.resource.get();
        if (res != nullptr && res->GetInitData() != nullptr) {
            const std::string& path = res->GetInitData()->Path;
            const size_t n = path.size();
            const size_t take = n < sizeof(info.tail) - 1 ? n : sizeof(info.tail) - 1;
            memcpy(info.tail, path.c_str() + (n - take), take);
            info.tail[take] = 0;
        } else {
            snprintf(info.tail, sizeof(info.tail), "@%08lx", (unsigned long)(uintptr_t)lt.addr);
        }
        GdxTrectNoteTexture(k.texAddr, k.texSize, &info);
    }

    sTrectPendSameRun = sameRun;
    sTrectPendFlushedBetween = sameRun && sTrectFlushSeq != sTrectFlushSeqAtPrevEnd;
    sTrectPendCls = kTrectClsOther;
    sTrectPendCause = 0;
    if (sameRun) {
        bool texDiffers = false;
        const int g = GdxTrectDiffGroup(sTrectPrev, k, &texDiffers);
        if (g >= 0) {
            sTrectPendCls = kTrectClsOther;
            sTrectPendCause = g;
        } else {
            sTrectPendCls = texDiffers ? kTrectClsTexOnly : kTrectClsIdent;
        }
    }
    // Atlas safety of THIS rect's tile: clamp-addressed (or mask 0 = implicit clamp) on both
    // axes, no mirror, and the rect's texel window inside the tile's [uls, lrs+1] x [ult, lrt+1].
    const bool clampS = ((tt.cms & G_TX_CLAMP) != 0 || tt.masks == 0) && !((tt.cms & G_TX_MIRROR) != 0 && tt.masks != 0);
    const bool clampT = ((tt.cmt & G_TX_CLAMP) != 0 || tt.maskt == 0) && !((tt.cmt & G_TX_MIRROR) != 0 && tt.maskt != 0);
    sTrectPendClamp = clampS && clampT;
    const float s0 = (float)uls / 32.0f, s1 = lrs / 32.0f;
    const float t0 = (float)ult / 32.0f, t1 = lrt / 32.0f;
    const float sMin = s0 < s1 ? s0 : s1, sMax = s0 < s1 ? s1 : s0;
    const float tMin = t0 < t1 ? t0 : t1, tMax = t0 < t1 ? t1 : t0;
    sTrectPendUvIn = sMin >= tt.uls / 4.0f - 0.01f && sMax <= tt.lrs / 4.0f + 1.01f &&
                     tMin >= tt.ult / 4.0f - 0.01f && tMax <= tt.lrt / 4.0f + 1.01f; // 10.2 fixed tile bounds

    sTrectPrev = k;
    sTrectPrevValid = true;
    sTrectFlushSeqAtEnter = sTrectFlushSeq;
}

void GdxTrectExit() {
    const bool flushedIn = sTrectFlushSeq != sTrectFlushSeqAtEnter;
    if (flushedIn) {
        sTrect.flushIn++;
    }
    if (sTrectPendSameRun) {
        if (sTrectPendFlushedBetween) {
            sTrect.flushBetween++;
        }
        const uint32_t flushed = (flushedIn || sTrectPendFlushedBetween) ? 1u : 0u;
        switch (sTrectPendCls) {
            case kTrectClsIdent:
                sTrect.ident++;
                sTrect.identFlushed += flushed;
                break;
            case kTrectClsTexOnly:
                sTrect.texOnly++;
                sTrect.texOnlyFlushed += flushed;
                sTrect.texOnlyClamp += sTrectPendClamp ? 1u : 0u;
                sTrect.texOnlyUvIn += (sTrectPendClamp && sTrectPendUvIn) ? 1u : 0u;
                break;
            default:
                sTrect.other++;
                sTrect.otherFlushed += flushed;
                sTrect.cause[sTrectPendCause]++;
                break;
        }
    }
    sTrectFlushSeqAtPrevEnd = sTrectFlushSeq;
    sTrectTriSeqAtPrevEnd = sTrectTriSeq;
}
} // namespace

// Drain: formats two lines (GpuProfLog's buffer is 224 bytes) and resets the window. Returns
// 0 when no rect was censused. Per-frame figures divide by `frames`; hist/dist are window
// totals. svcGetSystemTick runs at SYSCLOCK_ARM11 = 268111856 Hz on every 3DS model.
extern "C" int gdx_trect_census_format(char* line1, char* line2, int cap, unsigned frames) {
    if (sTrect.rects == 0) {
        return 0;
    }
    const double inv = frames != 0 ? 1.0 / (double)frames : 1.0;
    const double ticksPerMs = 268111.856;
    const double rectMs = (double)sTrect.rectTicks / ticksPerMs * inv;
    unsigned long atlasStats[5] = { 0, 0, 0, 0, 0 };
    if (&gdx3ds_atlas_stats != nullptr) {
        gdx3ds_atlas_stats(atlasStats);
    }
    const double impMs = (double)sTrect.impTicks / ticksPerMs * inv;
    const double drawMs = (double)sTrect.flushTicks / ticksPerMs * inv;
    snprintf(line1, (size_t)cap,
             "[trect] n=%u r/f=%.1f runs/f=%.1f ident=%.1f/%.1f tex=%.1f/%.1f clamp=%.1f uvin=%.1f "
             "oth=%.1f/%.1f om=%u cc=%u pe=%u sc=%u tl=%u fIn/f=%.1f fBtw/f=%.1f imp/f=%.1f",
             frames, sTrect.rects * inv, sTrect.runs * inv, sTrect.ident * inv, sTrect.identFlushed * inv,
             sTrect.texOnly * inv, sTrect.texOnlyFlushed * inv, sTrect.texOnlyClamp * inv,
             sTrect.texOnlyUvIn * inv, sTrect.other * inv, sTrect.otherFlushed * inv, sTrect.cause[0],
             sTrect.cause[1], sTrect.cause[2], sTrect.cause[3], sTrect.cause[4], sTrect.flushIn * inv,
             sTrect.flushBetween * inv, sTrect.impCalls * inv);
    snprintf(line2, (size_t)cap,
             "[trect2] dist=%u/%uKB hist=%u,%u,%u,%u,%u maxrun=%u/%u ms/f rect=%.2f imp=%.2f draw=%.2f "
             "rest=%.2f tb=%d/%u/%u/%u/%u vt=%u atlas=%lu/%lu/%lu/%lu lin=%luKB",
             sTrectWinDistinct, sTrectWinBytes / 1024u, sTrect.runHist[0], sTrect.runHist[1], sTrect.runHist[2],
             sTrect.runHist[3], sTrect.runHist[4], sTrect.maxRunLen, sTrectRunDistinct > sTrect.maxRunDistinct
                                                                        ? sTrectRunDistinct
                                                                        : sTrect.maxRunDistinct,
             rectMs, impMs, drawMs, rectMs - impMs - drawMs, sTrectBatchOn ? 1 : 0, sTrectBatchMerged,
             sTrectBatchSwitched, sTrectBatchBypassed, sTrectBatchArmed, sTrectViewTriDraws, atlasStats[0],
             atlasStats[1], atlasStats[2], atlasStats[3], atlasStats[4]);
    sTrectBatchMerged = sTrectBatchSwitched = sTrectBatchBypassed = sTrectBatchArmed = sTrectViewTriDraws = 0;
    // [trect-tex] one-shot inventory dumps of the biggest rect-texture sets seen (menus and the
    // race-start / results crowd), 4 windows total, 3 entries per line, budgeted.
    if (sTrectDumpsLeft > 0 && sTrectWinDistinct >= 20 && gdx_trect_census_dump != nullptr) {
        sTrectDumpsLeft--;
        char line[224];
        int len = 0;
        const uint32_t n = sTrectWinDistinct < kTrectWinTexCap ? sTrectWinDistinct : kTrectWinTexCap;
        len = snprintf(line, sizeof(line), "[trect-tex] win=%u distinct=%u:", sTrectDumpsLeft, sTrectWinDistinct);
        gdx_trect_census_dump(line);
        for (uint32_t i = 0; i < n && i < 96; i++) {
            const GdxTrectTexInfo& t = sTrectWinInfo[i];
            len = 0;
            line[0] = 0;
            for (int j = 0; j < 3 && i < n && i < 96; j++, i++) {
                const GdxTrectTexInfo& u = sTrectWinInfo[i];
                len += snprintf(line + len, sizeof(line) - (size_t)len, " %s f%u/%u %ux%u b%u c%u%u t%u p%u |", u.tail,
                                u.fmt, u.siz, u.w, u.h, u.bytes, u.cms, u.cmt, u.filt, u.palIdx);
            }
            i--;
            (void)t;
            gdx_trect_census_dump(line);
        }
    }
    memset(&sTrect, 0, sizeof(sTrect));
    sTrectWinDistinct = 0;
    sTrectWinBytes = 0;
    return 1;
}
#endif

// [texrect] Full invalidation for events that can invalidate ANY banked draw state (task/frame
// boundaries, texture/shader cache clears — freed comb/prg pointers, nulled mRenderingState
// textures, re-armed imports): both memos die and every dirty bit arms.
static inline void GfxS7InvalidateDrawMemos() {
    sGfxS7TriMemo.valid = false;
    sGfxS7RectMemo.valid = false;
    sGfxS7DirtyMask = 0xFFu;
#ifdef __3DS__
    sTrectPrevValid = false; // [trect] task/frame boundary ends the rect run
    GdxTrectBatchLatch();    // [trectbatch] killswitch re-read at task/frame boundaries
#endif
}

// [perf-s7] Compute gate for the per-vertex / per-triangle geometry diagnostics (NDC min/max
// divides, isfinite sweeps, big-tri extent scans, fog-factor ranges). Every consumer of those
// fields ([geodiag]/[bigtri]/[phasegeom]/[gpustate]) prints only under gdx_diag_verbose(), so
// skipping the arithmetic while that gate is off is observationally identical — it just stops
// charging ~3 FP divides + 4 isfinite + 6 min/max per vertex to every release-run frame.
// Seeded from the same env var here (desktop parity); the port bridge mirrors the live
// Dev-Tools gate into it once per gfx task, so the F1 toggle still works mid-run.
static int GdxS7GeoDiagInitFromEnv() {
    const char* e = std::getenv("GDX_DIAG_VERBOSE");
    return (e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
}
extern "C" int gdx_s7_geo_diag_enabled = GdxS7GeoDiagInitFromEnv();
// Set a cached pointer to the instance so we don't need to go through the window every time
void GfxSetInstance(std::shared_ptr<Interpreter> gfx) {
    mInstance = gfx;
    sGfxInstanceRaw = gfx.get();
}

// N64 prim_depth is 15-bit (0 near, 0x7FFF far).
static constexpr float N64_PRIM_DEPTH_MAX = 32767.0f;

#ifdef __3DS__
// [triloop] Per-batch packed-emission state (single interpreter instance — the same
// assumption sGfxInstanceRaw already makes). A batch chooses its mode at its FIRST
// tri append (mBufVboNumTris == 0): gdx3ds_vbopack_begin either hands back a write
// cursor into the backend's linear VBO pool plus the batch's pack parameters, or
// refuses (killswitch, no shader, frame inactive, pool nearly exhausted), in which
// case the WHOLE batch takes the legacy variable-stride path. The mode can never
// need to change mid-batch: every state mutation that feeds the pack parameters
// (shader, texture binds/samplers, prim/env/fog/blend colours, fog line) already
// splits the batch with a Flush() — the same invariant the s7 memo, the vertex-0
// stage constants and the fog latch rely on.
static float* sGdxPackDst = nullptr;
static bool sGdxPackBatch = false;
static Gdx3dsVboPackParams sGdxPackParams;
static Gdx3dsVboPackAux sGdxPackAux;
// [native] fx_hud: the generic 2D-batcher rule. The citro3d backend carries ONE combiner input
// per channel in the vertex colour attribute and binds every other input as a TexEnv stage
// constant from vertex 0, which is why GfxDpSetPrimColor/GfxDpSetEnvColor split the batch on a
// value change. When the backend reports (Gdx_NativeBatchConstInputs) that no stage constant of
// the open packed batch's shader reads the changed register, the register is per-vertex: the
// packed append evaluates it per vertex at append time (gdxInput below), so NOT splitting yields
// the same VBO bytes and the same stage constants, in one draw instead of N. Stars are the
// headline case (100 rects, one texture, a prim colour per star: 100 draws -> 1). Prim DEPTH keeps
// its own flush (gfx_set_prim_depth_handler_rdp), so the stereo anchor split is untouched.
static const void* sGdxPackComb = nullptr; // ColorCombiner the open packed batch was begun under
static int sGdxHudFold = -1;               // [native] fx_hud, latched once the ini is loaded
static uint32_t sGdxHudFoldPrim = 0;
// A batch that absorbed a fold must NOT take the [trectbatch] deferred import (atlas view
// merge) at its next texture change: on the stock path the prim flush had emptied the batch,
// so that import ran on an empty batch (plain Flush + import). Keeping that order keeps the
// draw split and the import path identical; only the split moves from the colour change
// to the texture change. (Without this, the first frame of course select differed: the
// glyph rects went through the view-merge path, which is not pixel-transparent.)
static bool sGdxPackFolded = false;
static uint32_t sGdxHudFoldEnv = 0;
extern "C" int gdx3ds_native_ready(void) __attribute__((weak));
extern "C" int gdx3ds_native_lever(const char* key, int fallback) __attribute__((weak));
extern "C" int Gdx_NativeBatchConstInputs(uint32_t* rgbMask, uint32_t* alphaMask) __attribute__((weak));
extern "C" void gdx3ds_hud_fold_stats(uint32_t out[2]) {
    out[0] = sGdxHudFoldPrim;
    out[1] = sGdxHudFoldEnv;
}
static bool GdxHudMuxIsReg(uint8_t mux, bool isPrim, bool alphaChannel) {
    if (isPrim) {
        if (alphaChannel) {
            return mux == G_ACMUX_PRIMITIVE || mux == G_ACMUX_PRIM_LOD_FRAC;
        }
        return mux == G_CCMUX_PRIMITIVE || mux == G_CCMUX_PRIMITIVE_ALPHA || mux == G_CCMUX_PRIM_LOD_FRAC;
    }
    if (alphaChannel) {
        return mux == G_ACMUX_ENVIRONMENT;
    }
    return mux == G_CCMUX_ENVIRONMENT || mux == G_CCMUX_ENV_ALPHA;
}
// True when a value change of prim (env) colour can be absorbed by the OPEN packed batch.
static bool GdxHudRegFoldable(bool isPrim) {
    if (sGdxHudFold < 0) {
        if (&gdx3ds_native_ready == nullptr || !gdx3ds_native_ready() || &gdx3ds_native_lever == nullptr) {
            return false;
        }
        sGdxHudFold = gdx3ds_native_lever("fx_hud", 0) != 0 ? 1 : 0;
    }
    if (sGdxHudFold == 0 || !sGdxPackBatch || sGdxPackComb == nullptr || &Gdx_NativeBatchConstInputs == nullptr) {
        return false;
    }
    uint32_t rgbMask = 0, alphaMask = 0;
    if (!Gdx_NativeBatchConstInputs(&rgbMask, &alphaMask)) {
        return false;
    }
    const ColorCombiner* comb = static_cast<const ColorCombiner*>(sGdxPackComb);
    for (int j = 0; j < 7; j++) {
        if (((rgbMask >> j) & 1u) && GdxHudMuxIsReg(comb->shader_input_mapping[0][j], isPrim, false)) {
            return false;
        }
        if (((alphaMask >> j) & 1u) && GdxHudMuxIsReg(comb->shader_input_mapping[1][j], isPrim, true)) {
            return false;
        }
    }
    const int ai = sGdxPackParams.vtxAlphaInput - 1;
    if (ai >= 0 && GdxHudMuxIsReg(comb->shader_input_mapping[1][ai], isPrim, true) &&
        (!sGdxPackParams.wantVtxAlpha || sGdxPackParams.fogFactorToAlpha)) {
        return false;
    }
    return true;
}
#endif

void Interpreter::Flush() {
    if (mBufVboLen > 0) {
        mGeometryDiagnostics.gpuDrawCalls++;
        mGeometryDiagnostics.gpuTriangles += mBufVboNumTris;
        mRapi->SetCurrentPrimDepth((float)mRdp->prim_depth / N64_PRIM_DEPTH_MAX);
        // G_AC_THRESHOLD compares against the SETBLENDCOLOR alpha register, not a constant.
        mRapi->SetCurrentAlphaCompareThreshold((float)mRdp->blend_color.a / 255.0f);
#ifdef __3DS__
        // [trect] census: time the submit when it lands inside a texrect, count every submit.
        const bool trectTime = sTrectInRect && gdx3ds_prof_active != 0;
        const long long trectT0 = trectTime ? gdx3ds_prof_now() : 0;
        // [triloop] Packed batches were emitted straight into the linear VBO pool in
        // the final PICA layout; submit repack-free with the batch aux record.
        if (sGdxPackBatch) {
            gdx3ds_vbopack_draw(mBufVboNumTris, &sGdxPackAux);
            sGdxPackBatch = false;
            sGdxPackDst = nullptr;
        } else {
            mRapi->DrawTriangles(mBufVbo, mBufVboLen, mBufVboNumTris);
        }
        if (trectTime) {
            sTrect.flushTicks += (uint64_t)(gdx3ds_prof_now() - trectT0);
        }
        sTrectFlushSeq++;
#else
        mRapi->DrawTriangles(mBufVbo, mBufVboLen, mBufVboNumTris);
#endif
        mBufVboLen = 0;
        mBufVboNumTris = 0;
#ifdef __3DS__
        sGdxPackFolded = false;
#endif
    }
}

// Bit-exact on purpose: a tolerance would hide the sub-ULP drift that flips a triangle
// across a clip plane.
static inline void GdxHashFloat(uint64_t& h, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    h ^= static_cast<uint64_t>(bits);
    h *= 0x100000001B3ull;
}

// Runs per vertex per pass (~700k hashes/sec in normal play), too expensive to leave on.
static const bool sGdxVertexHashEnabled = [] {
    const char* e = std::getenv("GDX_INTERP_GEO");
    return e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
}();

void Interpreter::ResetGeometryDiagnostics() {
    mGeometryDiagnostics = {};
    mGeometryDiagnostics.vertexHash = 0xCBF29CE484222325ull;  // FNV-1a offset basis
    mGeometryDiagnostics.mpFirstHash = 0xCBF29CE484222325ull;
    mGeometryDiagnostics.minNdcX = std::numeric_limits<float>::infinity();
    mGeometryDiagnostics.minNdcY = std::numeric_limits<float>::infinity();
    mGeometryDiagnostics.minNdcZ = std::numeric_limits<float>::infinity();
    mGeometryDiagnostics.maxNdcX = -std::numeric_limits<float>::infinity();
    mGeometryDiagnostics.maxNdcY = -std::numeric_limits<float>::infinity();
    mGeometryDiagnostics.maxNdcZ = -std::numeric_limits<float>::infinity();
}

const GeometryDiagnostics& Interpreter::GetGeometryDiagnostics() const {
    return mGeometryDiagnostics;
}

void Interpreter::SetF3dex2Variant(F3dex2Variant variant) {
    if (mF3dex2Variant != variant) {
        mGeometryDiagnostics.variantSwitches++;
        if (variant == F3dex2Variant::FZeroFlxReject) {
            mGeometryDiagnostics.preFlxVertices = mGeometryDiagnostics.verticesLoaded;
            mGeometryDiagnostics.preFlxTrianglesSubmitted = mGeometryDiagnostics.trianglesSubmitted;
            mGeometryDiagnostics.preFlxTrianglesEmitted = mGeometryDiagnostics.trianglesEmitted;
            mGeometryDiagnostics.preFlxGpuDrawCalls = mGeometryDiagnostics.gpuDrawCalls;
            mGeometryDiagnostics.preFlxGpuTriangles = mGeometryDiagnostics.gpuTriangles;
            mGeometryDiagnostics.preFlxRgba16OpaquePixels = mGeometryDiagnostics.rgba16OpaquePixels;
            mGeometryDiagnostics.preFlxRgba16TransparentPixels = mGeometryDiagnostics.rgba16TransparentPixels;
            mGeometryDiagnostics.preFlxRgba16ForcedOpaquePixels =
                mGeometryDiagnostics.rgba16ForcedOpaquePixels;
            mGeometryDiagnostics.preFlxDepthBypassTriangles = mGeometryDiagnostics.depthBypassTriangles;
            mGeometryDiagnostics.preFlxTexturedTriangles = mGeometryDiagnostics.texturedTriangles;
            mGeometryDiagnostics.preFlxTexture0BoundTriangles = mGeometryDiagnostics.texture0BoundTriangles;
            mGeometryDiagnostics.preFlxTexture0MissingTriangles = mGeometryDiagnostics.texture0MissingTriangles;
            mGeometryDiagnostics.preFlxForcedSimpleMaterialTriangles =
                mGeometryDiagnostics.forcedSimpleMaterialTriangles;
            mGeometryDiagnostics.preFlxShaderId0 = mGeometryDiagnostics.lastShaderId0;
            mGeometryDiagnostics.preFlxShaderId1 = mGeometryDiagnostics.lastShaderId1;
            mGeometryDiagnostics.preFlxTextureWidth = mGeometryDiagnostics.textureWidth;
            mGeometryDiagnostics.preFlxTextureHeight = mGeometryDiagnostics.textureHeight;
            mGeometryDiagnostics.preFlxTextureLineBytes = mGeometryDiagnostics.textureLineBytes;
            mGeometryDiagnostics.preFlxTextureSizeBytes = mGeometryDiagnostics.textureSizeBytes;
            mGeometryDiagnostics.preFlxTextureTmem = mGeometryDiagnostics.textureTmem;
            mGeometryDiagnostics.preFlxTextureTile = mGeometryDiagnostics.textureTile;
            mGeometryDiagnostics.preFlxTextureMaskS = mGeometryDiagnostics.textureMaskS;
            mGeometryDiagnostics.preFlxTextureMaskT = mGeometryDiagnostics.textureMaskT;
            mGeometryDiagnostics.preFlxTextureScaleS = mGeometryDiagnostics.textureScaleS;
            mGeometryDiagnostics.preFlxTextureScaleT = mGeometryDiagnostics.textureScaleT;
            mGeometryDiagnostics.preFlxFogTriangles = mGeometryDiagnostics.fogTriangles;
            mGeometryDiagnostics.preFlxFogBypassTriangles = mGeometryDiagnostics.fogBypassTriangles;
            mGeometryDiagnostics.preFlxMinFogFactor = mGeometryDiagnostics.minFogFactor;
            mGeometryDiagnostics.preFlxMaxFogFactor = mGeometryDiagnostics.maxFogFactor;
            mGeometryDiagnostics.preFlxFogMul = mRsp->fog_mul;
            mGeometryDiagnostics.preFlxFogOffset = mRsp->fog_offset;
            mGeometryDiagnostics.preFlxMinTextureU = mGeometryDiagnostics.minTextureU;
            mGeometryDiagnostics.preFlxMaxTextureU = mGeometryDiagnostics.maxTextureU;
            mGeometryDiagnostics.preFlxMinTextureV = mGeometryDiagnostics.minTextureV;
            mGeometryDiagnostics.preFlxMaxTextureV = mGeometryDiagnostics.maxTextureV;
            mGeometryDiagnostics.preFlxOtherModeH = mRdp->other_mode_h;
            mGeometryDiagnostics.preFlxOtherModeL = mRdp->other_mode_l;
            mGeometryDiagnostics.preFlxCombineMode = mRdp->combine_mode;
            mGeometryDiagnostics.preFlxTexture = mRdp->LoadedTextureAt(0).addr;
        }
        mRsp->f3dflx_alpha_light_valid = false;
    }
    mF3dex2Variant = variant;
}

ShaderProgram* Interpreter::LookupOrCreateShaderProgram(uint64_t id0, uint64_t id1) {
    ShaderProgram* prg = mRapi->LookupShader(id0, id1);
    if (prg == nullptr) {
        mRapi->UnloadShader(mRenderingState.mShaderProgram);
        prg = mRapi->CreateAndLoadNewShader(id0, id1);
        mRenderingState.mShaderProgram = prg;
    }
    return prg;
}

const char* Interpreter::CCMUXtoStr(uint32_t ccmux) {
    static constexpr std::array tbl = {
        "G_CCMUX_COMBINED",
        "G_CCMUX_TEXEL0",
        "G_CCMUX_TEXEL1",
        "G_CCMUX_PRIMITIVE",
        "G_CCMUX_SHADE",
        "G_CCMUX_ENVIRONMENT",
        "G_CCMUX_1",
        "G_CCMUX_COMBINED_ALPHA",
        "G_CCMUX_TEXEL0_ALPHA",
        "G_CCMUX_TEXEL1_ALPHA",
        "G_CCMUX_PRIMITIVE_ALPHA",
        "G_CCMUX_SHADE_ALPHA",
        "G_CCMUX_ENV_ALPHA",
        "G_CCMUX_LOD_FRACTION",
        "G_CCMUX_PRIM_LOD_FRAC",
        "G_CCMUX_K5",
    };
    if (ccmux > tbl.size()) {
        return "G_CCMUX_0";
    }
    return tbl[ccmux];
}

// Seems unused
const char* Interpreter::ACMUXtoStr(uint32_t acmux) {
    static constexpr std::array tbl = {
        "G_ACMUX_COMBINED or G_ACMUX_LOD_FRACTION",
        "G_ACMUX_TEXEL0",
        "G_ACMUX_TEXEL1",
        "G_ACMUX_PRIMITIVE",
        "G_ACMUX_SHADE",
        "G_ACMUX_ENVIRONMENT",
        "G_ACMUX_1 or G_ACMUX_PRIM_LOD_FRAC",
        "G_ACMUX_0",
    };
    return tbl[acmux];
}

void Interpreter::GenerateCC(ColorCombiner* comb, const ColorCombinerKey& key) {
    const bool is2Cyc = (key.options & SHADER_OPT(_2CYC)) != 0;

    uint8_t c[2][2][4];
    uint64_t shaderId0 = 0;
    uint64_t shaderId1 = key.options;
    uint8_t shaderInputMapping[2][7] = { { 0 } };
    bool usedTextures[2]{};
    for (uint32_t i = 0; i < 2 && (i == 0 || is2Cyc); i++) {
        uint32_t rgbA = (key.combine_mode >> (i * 28)) & 0xf;
        uint32_t rgbB = (key.combine_mode >> (i * 28 + 4)) & 0xf;
        uint32_t rgbC = (key.combine_mode >> (i * 28 + 8)) & 0x1f;
        uint32_t rgbD = (key.combine_mode >> (i * 28 + 13)) & 7;
        uint32_t alphaA = (key.combine_mode >> (i * 28 + 16)) & 7;
        uint32_t alphaB = (key.combine_mode >> (i * 28 + 16 + 3)) & 7;
        uint32_t alphaC = (key.combine_mode >> (i * 28 + 16 + 6)) & 7;
        uint32_t alphaD = (key.combine_mode >> (i * 28 + 16 + 9)) & 7;

        if (rgbA >= 8) {
            rgbA = G_CCMUX_0;
        }
        if (rgbB >= 8) {
            rgbB = G_CCMUX_0;
        }
        if (rgbC >= 16) {
            rgbC = G_CCMUX_0;
        }
        if (rgbD == 7) {
            rgbD = G_CCMUX_0;
        }

        if (rgbA == rgbB || rgbC == G_CCMUX_0) {
            // Normalize
            rgbA = G_CCMUX_0;
            rgbB = G_CCMUX_0;
            rgbC = G_CCMUX_0;
        }
        if (alphaA == alphaB || alphaC == G_ACMUX_0) {
            // Normalize
            alphaA = G_ACMUX_0;
            alphaB = G_ACMUX_0;
            alphaC = G_ACMUX_0;
        }
        if (i == 1) {
            if (rgbA != G_CCMUX_COMBINED && rgbB != G_CCMUX_COMBINED && rgbC != G_CCMUX_COMBINED &&
                rgbD != G_CCMUX_COMBINED) {
                // First cycle RGB not used, so clear it away
                c[0][0][0] = c[0][0][1] = c[0][0][2] = c[0][0][3] = G_CCMUX_0;
            }
            if (rgbC != G_CCMUX_COMBINED_ALPHA && alphaA != G_ACMUX_COMBINED && alphaB != G_ACMUX_COMBINED &&
                alphaD != G_ACMUX_COMBINED) {
                // First cycle ALPHA not used, so clear it away
                c[0][1][0] = c[0][1][1] = c[0][1][2] = c[0][1][3] = G_ACMUX_0;
            }
        }

        c[i][0][0] = rgbA;
        c[i][0][1] = rgbB;
        c[i][0][2] = rgbC;
        c[i][0][3] = rgbD;
        c[i][1][0] = alphaA;
        c[i][1][1] = alphaB;
        c[i][1][2] = alphaC;
        c[i][1][3] = alphaD;
    }
    if (!is2Cyc) {
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t k = 0; k < 4; k++) {
                c[1][i][k] = i == 0 ? G_CCMUX_0 : G_ACMUX_0;
            }
        }

        // In 1-cycle mode, TEXEL1 returns the same value as TEXEL0.
        // Remap combiner inputs so the shader samples from the correct slot.
        // Ex: TEXEL1/TEXEL1_ALPHA → TEXEL0/TEXEL0_ALPHA
        for (uint32_t k = 0; k < 4; k++) {
            if (c[0][0][k] == G_CCMUX_TEXEL1)
                c[0][0][k] = G_CCMUX_TEXEL0;
            if (c[0][0][k] == G_CCMUX_TEXEL1_ALPHA)
                c[0][0][k] = G_CCMUX_TEXEL0_ALPHA;
            if (c[0][1][k] == G_ACMUX_TEXEL1)
                c[0][1][k] = G_ACMUX_TEXEL0;
        }
    }
    {
        uint8_t inputNumber[32] = { 0 };
        uint32_t nextInputNumber = SHADER_INPUT_1;
        for (uint32_t i = 0; i < 2 && (i == 0 || is2Cyc); i++) {
            for (uint32_t j = 0; j < 4; j++) {
                // Mux values 6/7/15 are overloaded by slot. Only B (value 6 = CENTER, 7 = K4)
                // and C (value 6 = SCALE, 15 = K5) carry chroma-key/convert inputs; A and D
                // reuse those values for unrelated constants.
                if (j == 1 && c[i][0][j] == G_CCMUX_CENTER) {
                    c[i][0][j] = G_CCMUX_KEY_CENTER;
                } else if (j == 1 && c[i][0][j] == G_CCMUX_K4) {
                    c[i][0][j] = G_CCMUX_CONVERT_K4;
                } else if (j == 2 && c[i][0][j] == G_CCMUX_SCALE) {
                    c[i][0][j] = G_CCMUX_KEY_SCALE;
                } else if (j == 2 && c[i][0][j] == G_CCMUX_K5) {
                    c[i][0][j] = G_CCMUX_CONVERT_K5;
                }
                uint32_t val = 0;
                switch (c[i][0][j]) {
                    case G_CCMUX_0:
                        val = SHADER_0;
                        break;
                    case G_CCMUX_1:
                        val = SHADER_1;
                        break;
                    case G_CCMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        // Set the opposite texture when reading from the second cycle color options
                        if (i == 0) {
                            usedTextures[0] = true;
                        } else {
                            usedTextures[1] = true;
                        }
                        break;
                    case G_CCMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        if (i == 0) {
                            usedTextures[1] = true;
                        } else {
                            usedTextures[0] = true;
                        }
                        break;
                    case G_CCMUX_TEXEL0_ALPHA:
                        val = SHADER_TEXEL0A;
                        if (i == 0) {
                            usedTextures[0] = true;
                        } else {
                            usedTextures[1] = true;
                        }
                        break;
                    case G_CCMUX_TEXEL1_ALPHA:
                        val = SHADER_TEXEL1A;
                        if (i == 0) {
                            usedTextures[1] = true;
                        } else {
                            usedTextures[0] = true;
                        }
                        break;
                    case G_CCMUX_NOISE:
                        val = SHADER_NOISE;
                        break;
                    case G_CCMUX_PRIMITIVE:
                    case G_CCMUX_PRIMITIVE_ALPHA:
                    case G_CCMUX_PRIM_LOD_FRAC:
                    case G_CCMUX_SHADE:
                    case G_CCMUX_SHADE_ALPHA:
                    case G_CCMUX_ENVIRONMENT:
                    case G_CCMUX_ENV_ALPHA:
                    case G_CCMUX_LOD_FRACTION:
                    case G_CCMUX_KEY_CENTER:
                    case G_CCMUX_KEY_SCALE:
                    case G_CCMUX_CONVERT_K4:
                    case G_CCMUX_CONVERT_K5:
                        if (inputNumber[c[i][0][j]] == 0) {
                            shaderInputMapping[0][nextInputNumber - 1] = c[i][0][j];
                            inputNumber[c[i][0][j]] = nextInputNumber++;
                        }
                        val = inputNumber[c[i][0][j]];
                        break;
                    case G_CCMUX_COMBINED:
                        val = SHADER_COMBINED;
                        break;
                    default:
                        fprintf(stderr, "Unsupported ccmux: %d\n", c[i][0][j]);
                        break;
                }
                shaderId0 |= (uint64_t)val << (i * 32 + j * 4);
            }
        }
    }
    {
        uint8_t inputNumber[16] = { 0 };
        uint32_t nextInputNumber = SHADER_INPUT_1;
        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][1][j]) {
                    case G_ACMUX_0:
                        val = SHADER_0;
                        break;
                    case G_ACMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        // Set the opposite texture when reading from the second cycle color options
                        if (i == 0) {
                            usedTextures[0] = true;
                        } else {
                            usedTextures[1] = true;
                        }
                        break;
                    case G_ACMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        if (i == 0) {
                            usedTextures[1] = true;
                        } else {
                            usedTextures[0] = true;
                        }
                        break;
                    case G_ACMUX_LOD_FRACTION:
                        // case G_ACMUX_COMBINED: same numerical value
                        if (j != 2) {
                            val = SHADER_COMBINED;
                            break;
                        }
                        c[i][1][j] = G_CCMUX_LOD_FRACTION;
                        [[fallthrough]]; // for G_ACMUX_LOD_FRACTION
                    case G_ACMUX_1:
                        // case G_ACMUX_PRIM_LOD_FRAC: same numerical value
                        if (j != 2) {
                            val = SHADER_1;
                            break;
                        }
                        [[fallthrough]]; // for G_ACMUX_PRIM_LOD_FRAC
                    case G_ACMUX_PRIMITIVE:
                    case G_ACMUX_SHADE:
                    case G_ACMUX_ENVIRONMENT:
                        if (inputNumber[c[i][1][j]] == 0) {
                            shaderInputMapping[1][nextInputNumber - 1] = c[i][1][j];
                            inputNumber[c[i][1][j]] = nextInputNumber++;
                        }
                        val = inputNumber[c[i][1][j]];
                        break;
                }
                shaderId0 |= (uint64_t)val << (i * 32 + 16 + j * 4);
            }
        }
    }
    comb->shader_id0 = shaderId0;
    comb->shader_id1 = shaderId1;
    comb->usedTextures[0] = usedTextures[0];
    comb->usedTextures[1] = usedTextures[1];
    // comb->prg = gfx_lookup_or_create_mShaderProgram(shader_id0, shader_id1);
    memcpy(comb->shader_input_mapping, shaderInputMapping, sizeof(shaderInputMapping));
}

ColorCombiner* Interpreter::LookupOrCreateColorCombiner(const ColorCombinerKey& key) {
    if (mPrevCombiner != mColorCombinerPool.end() && mPrevCombiner->first == key) {
        return &mPrevCombiner->second;
    }
    mPrevCombiner = mColorCombinerPool.find(key);
    if (mPrevCombiner != mColorCombinerPool.end()) {
        return &mPrevCombiner->second;
    }
    Flush();
    mPrevCombiner = mColorCombinerPool.insert(std::make_pair(key, ColorCombiner())).first;
    GenerateCC(&mPrevCombiner->second, key);
    return &mPrevCombiner->second;
}

extern "C" void gdx3ds_texcache_note_thread(int kind) __attribute__((weak));
static inline void GdxTexCacheNoteThread(int kind) {
    if (&gdx3ds_texcache_note_thread != nullptr) {
        gdx3ds_texcache_note_thread(kind);
    }
}

#ifdef __3DS__
/* [texclr] investigation receipt (residuals track): [debug] texclrdiag=1 mirrors the
 * pre-sort and post-sort head of free_texture_ids so two runs can be diffed. */
extern "C" int gdx_texclr_diag_on(void) __attribute__((weak));
extern "C" void gdx_texclr_dump(const char* line) __attribute__((weak));
static std::vector<uint32_t> sGdxTexClrPre;

/* DETERMINISM (residuals / residuals2 tracks, the frame-1702 residual).
 * free_texture_ids is consumed LIFO by TextureCacheLookup, so its ORDER decides which
 * recycled id a later import receives. With the [trectbatch] HUD atlas the id decides which
 * atlas CELL the texture is packed into (AtlasTryPlace), the cell decides the view's
 * uOff/uScale, and those decide the sub-texel sampling phase of every rect drawn from it —
 * so a different id means different bilinear weights at glyph edges.
 * Every producer that appends in unordered_map / bucket-chain order therefore has to leave
 * the vector canonically ordered, because TextureCacheKey::Hasher is addr ^ (addr >> 5) over
 * the texture's RAW GUEST POINTER and the guest heap layout is not pinned (the core-2 asset
 * preload shifts it run to run). Descending sort == "lowest free id first" under LIFO pops.
 * Whole vector, never just the appended tail: ids left over from an earlier batch must take
 * part or the order is only half pinned. Cost: one sort of at most TEXTURE_CACHE_MAX_SIZE
 * u32 per FREEING BATCH (mode changes, buffer refreshes, palette restages) — never per
 * texture and never on the steady draw path. */
static inline void GdxCanonicalizeFreeTextureIds(std::vector<uint32_t>& ids) {
    std::sort(ids.begin(), ids.end(), std::greater<uint32_t>());
}
#endif

void Interpreter::TextureCacheClear() {
#ifdef __3DS__
    GdxTexCacheNoteThread(3);
#endif
    GfxS7InvalidateDrawMemos(); // [perf-s7] mRenderingState.mTextures nulled below
    for (const auto& entry : mTextureCache.map) {
        mTextureCache.free_texture_ids.push_back(entry.second.texture_id);
    }
#ifdef __3DS__
    if (&gdx_texclr_diag_on != nullptr && gdx_texclr_diag_on() != 0) {
        sGdxTexClrPre = mTextureCache.free_texture_ids;
    }
    /* DETERMINISM (residuals track, frame-1702 residual). The loop above walks an
     * unordered_map whose bucket order is a function of the cached textures' raw source
     * POINTERS (TextureCacheKey::Hasher == addr ^ (addr >> 5)), and free_texture_ids is
     * consumed LIFO by TextureCacheLookup. The recycled id a later import receives was
     * therefore decided by hash-bucket order, which is not stable across otherwise
     * bit-identical replays. That was invisible while every texture owned its own pow2
     * C3D_Tex, but with the [trectbatch] HUD atlas the id decides which atlas CELL the
     * texture is packed into, the cell decides the view's uOff/uScale, and uOff/uScale
     * decide the sub-texel sampling phase of every rect drawn from it — so the course-name
     * glyphs rendered differently on the first frame they appear, and only there.
     * Sorting makes the free list a canonical descending sequence (LIFO pops ascending),
     * i.e. "lowest free id first", independent of map iteration order. See
     * GdxCanonicalizeFreeTextureIds above for the whole-vector rationale and the cost. */
    GdxCanonicalizeFreeTextureIds(mTextureCache.free_texture_ids);
    if (&gdx_texclr_diag_on != nullptr && gdx_texclr_diag_on() != 0 && &gdx_texclr_dump != nullptr) {
        char line[160];
        int n = snprintf(line, sizeof(line), "[texclr] n=%u pre=", (unsigned)mTextureCache.free_texture_ids.size());
        for (size_t i = 0; i < 8 && i < sGdxTexClrPre.size(); i++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%u,", (unsigned)sGdxTexClrPre[i]);
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " post=");
        for (size_t i = 0; i < 8 && i < mTextureCache.free_texture_ids.size(); i++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%u,", (unsigned)mTextureCache.free_texture_ids[i]);
        }
        gdx_texclr_dump(line);
    }
#endif
    mTextureCache.map.clear();
    mTextureCache.lru.clear();
    // Pre-allocate buckets so the map never rehashes during normal operation.
    // Rehashing invalidates all iterators, including those stored in LRU entries.
    mTextureCache.map.reserve(TEXTURE_CACHE_MAX_SIZE);
    // Null rendering-state pointers — they pointed into map nodes that are now freed.
    std::fill(std::begin(mRenderingState.mTextures), std::end(mRenderingState.mTextures), nullptr);
}

// 3DS texture-cache telemetry hooks (port/3ds/gfx/gfx_citro3d.cpp). The [c3d] texUp
// counter showed steady per-frame re-uploads in race scenes; these hooks let the
// backend name which lookups miss and how often invalidation fires. spdlog is a
// no-op stub on the 3DS build, so the evidence has to flow through the backend.
#ifdef __3DS__
extern "C" void gdx3ds_texcache_note_import(void);
/* RENDER THREAD ownership receipt (port/3ds/gdx3ds_renderthread.cpp): the texture cache
   (map + LRU list + free ids + draw memos) is render-thread-owned; every entry point reports
   its caller so a main-thread mutation shows up as [rt] texcacheMainMut= instead of a data
   abort inside the LRU splice. kind: 0 lookup, 1 delete, 2 delete-palette, 3 clear. */
extern "C" void gdx3ds_texcache_note_miss(const void* addr, unsigned fmt, unsigned siz, unsigned tmemWord,
                                          unsigned lineBytes, unsigned sizeBytes, unsigned tileW, unsigned tileH,
                                          unsigned contentHash);
extern "C" void gdx3ds_texcache_note_delete(unsigned evictedEntries);
// Exact fog line for the PICA fog LUT (port/3ds/gfx/gfx_citro3d.cpp UpdateFogState).
// The per-vertex fog factor baked into the vertex stream is clamp((z/w)*fog_mul +
// fog_offset, 0, 255); reconstructing that line from clamped endpoint samples smears
// steep fog curves (F-Zero X races: gSPFogPosition(990, 1000)) across the whole draw,
// painting the mid-range track in solid fog colour. The mirror is latched at
// fog-attribute APPEND time (the tri entering the batch), NOT at the G_MW_FOG
// moveword: movewords do not flush pending tris (per-vertex baking never needed it),
// so a moveword-time mirror hands the previous batch the NEXT draw's fog line —
// F-Zero X's race DL interleaves gSPFogPosition settings and that misbinding painted
// the whole track in the backdrop's linear fog (the "invisible road"). exact=0 marks
// constant-factor blend-colour draws, which must use the vertex-scan fit instead.
extern "C" void gdx3ds_fog_note_params(int mul, int off, int exact);
// SELECT-PERF SETTIMG-resolution counters (port/3ds/gfx/gfx_citro3d.cpp): note_load
// fires when the o2r-filepath SETTIMG handler takes the full LoadResourceProcess
// path, note_memo when the resolution is served from the SETTIMG memo below. The
// [gpu]/[c3d] lines surface them as rl=/rm= (dRl=/dRm=).
extern "C" void gdx3ds_settimg_note_load(void);
extern "C" void gdx3ds_settimg_note_memo(void);
// PRIM-COLOR (ship yellow-body): the 3DS backend serves draw-constant combiner
// inputs (PRIM/ENV colour) from per-stage TexEnv CONSTANT registers refreshed from
// vertex 0 of each flushed batch (gfx_citro3d.cpp RefreshStageConstants) — desktop
// GL reads them per-vertex and never needed a batch split on a colour change. So a
// G_SETPRIMCOLOR/G_SETENVCOLOR value change over a NON-EMPTY batch hands every
// pending tri the batch's FIRST colour: F-Zero X's race loop sets each machine's
// body colour through gDPSetEnvColor between same-combiner draws, and any merge
// window (untextured same-shader pieces, sampler-equal runs) painted the player's
// I4 body with a rival's constant — the yellow Blue Falcon. Same class as the
// G_MW_FOG moveword flush in lus-3ds-fog-exact-params.patch. The set handlers now
// flush pending tris when the VALUE changes (no-op re-sets do not split) and note
// the change here; filtering/arming/budget for the one-shot [prim] diag live
// port-side (INI [debug] diag_prim=1 / env GDX_DIAG_PRIM).
extern "C" void gdx3ds_prim_note_set(int isPrim, unsigned r, unsigned g, unsigned b, unsigned a,
                                     unsigned pendingTris, int flushed);
// SHIP-LIVERY-2 [livery] texture-identity receipts (port/3ds/gfx/gfx_citro3d.cpp).
// note_load fires on every TMEM store (StoreLoadedTexture) with the source's resource
// path when one exists; note_import fires on every ImportTexture resolution with the
// texture id the lookup BOUND for the consuming draw — the port keeps an id→identity
// table from these so its [livery] draw log can name the texture every in-race draw
// actually samples. Both are cheap early-returns while the port-side one-shot window
// is disarmed; the arming/budget/INI gate ([debug] diag_livery) live port-side.
extern "C" void gdx3ds_livery_note_load(unsigned tmemWord, const char* path, const void* addr,
                                        unsigned sizeBytes);
extern "C" void gdx3ds_livery_note_import(int unit, int tile, unsigned tmemWord, unsigned fmt,
                                          unsigned siz, unsigned texId, const char* path,
                                          const void* addr, unsigned tileW, unsigned tileH, int hit);
#endif

// [prof] sectioned CPU-build profiler hooks (port/3ds/gfx/gdx3ds_gpu_prof.c, gated on the
// latched debug.gputrace via gdx3ds_prof_active — one int load + branch per site when off).
// The interpreter owns the RUN/VTX/TRI/IMP/MTX buckets; BR (bridge pre-pass) and DRW
// (backend DrawTriangles) are recorded port-side. All buckets are EXCLUSIVE time: each
// scope snapshots the global child-tick counter on entry and subtracts whatever profiled
// children (a tri's state-resolve flush -> DrawTriangles, its ImportTexture, the clip-fan
// recursion into GfxSpTri1 itself) accumulated inside it, then reports its full wall delta
// upward — so the buckets sum without double-counting and Interpreter::Run's SELF time is
// exactly the dispatch remainder. Indices must match the gdx3ds_gpu_prof.h enum
// (BR=0 RUN=1 VTX=2 TRI=3 IMP=4 DRW=5 MTX=6, GDX3DS_PROF_SEC_COUNT=7).
#ifdef __3DS__
extern "C" {
extern int gdx3ds_prof_active;
extern uint64_t gdx3ds_prof_sec_ticks[7];
extern uint32_t gdx3ds_prof_sec_calls[7];
extern uint32_t gdx3ds_prof_vtx_loaded;
extern uint64_t gdx3ds_prof_child_ticks;
long long gdx3ds_prof_now(void);
// [profop] per-opcode handler tick/call accumulators, read+reset by gdx3ds_gpu_prof.c.
uint64_t gdx3ds_prof_op_ticks[256];
uint32_t gdx3ds_prof_op_calls[256];
}
namespace {
enum { kGdxProfRUN = 1, kGdxProfVTX = 2, kGdxProfTRI = 3, kGdxProfIMP = 4, kGdxProfMTX = 6 };
struct GdxProfScope {
    uint64_t t0;
    uint64_t childSnap;
    int sec;
    bool on;
    explicit GdxProfScope(int s) : sec(s), on(gdx3ds_prof_active != 0) {
        if (on) {
            childSnap = gdx3ds_prof_child_ticks;
            t0 = (uint64_t)gdx3ds_prof_now();
        }
    }
    ~GdxProfScope() {
        if (on) {
            const uint64_t dt = (uint64_t)gdx3ds_prof_now() - t0;
            gdx3ds_prof_sec_ticks[sec] += dt - (gdx3ds_prof_child_ticks - childSnap);
            gdx3ds_prof_sec_calls[sec]++;
            gdx3ds_prof_child_ticks = childSnap + dt;
        }
    }
};
} // namespace
#define GDX_PROF_SCOPE(sec) GdxProfScope gdxProfScope##sec(kGdxProf##sec)
#else
#define GDX_PROF_SCOPE(sec) \
    do {                    \
    } while (0)
#endif

#ifdef __3DS__
// [tri2] Per-phase census of GfxSpTri1 (LOCKED-60 Task C). Answers WHERE the ~8 us per
// triangle of the hardware [profop] 06 bucket goes: screening, state resolve, s7 memo,
// shader select, packed batch begin, vertex emission, tail. Gated on gputrace
// (gdx3ds_prof_active) AND the bridge-mirrored verbose gate (gdx_tri2_census_on), so a
// plain gputrace run pays one int load + branch per site. Ticks are svcGetSystemTick and
// EXCLUSIVE of every [prof]-scoped child (DRW inside Flush, IMP, nested clip-fan TRI)
// through the same child-tick ledger GdxProfScope keeps. Accumulators are drained and
// printed by the port bridge on the [race-dl] cadence as the `[tri2]` line.
extern "C" {
enum {
    kGdxTri2PhPre = 0, // entry -> reject box / clip pre-scan / clip_rej / cull screening
    kGdxTri2PhState,   // depth+decal+viewport probes, cc option derivation, combiner lookup
    kGdxTri2PhMemo,    // s7 tile-extent memo: hit copy-out or full re-derivation (+samplers)
    kGdxTri2PhPrg,     // shader program select/load + ShaderGetInfo/clip-parameter memo
    kGdxTri2PhBegin,   // packed batch begin + aux record (first tri of every batch only)
    kGdxTri2PhPack,    // clamp resolve + three-vertex emission (packed or legacy)
    kGdxTri2PhTail,    // emitted counters + batch-full flush check
    kGdxTri2PhCount
};
enum {
    kGdxTri2CntCalls = 0,
    kGdxTri2CntRect,
    kGdxTri2CntEarly,    // screened out (reject box, degenerate clip, clip_rej, cull)
    kGdxTri2CntFan,      // clipped: re-entered as a fan (nested calls counted separately)
    kGdxTri2CntMemoHit,
    kGdxTri2CntMemoMiss,
    kGdxTri2CntBatch,    // batches begun (== packed draws when the pool accepts)
    kGdxTri2CntPacked,   // tris emitted through the packed path
    kGdxTri2CntLegacy,   // tris emitted through the legacy variable-stride path
    kGdxTri2CntCount
};
uint64_t gdx_tri2_phase_ticks[kGdxTri2PhCount];
uint32_t gdx_tri2_cnt[kGdxTri2CntCount];
int gdx_tri2_census_on = 0;
}
namespace {
struct GdxTri2Phaser {
    bool on;
    uint64_t t;
    uint64_t child;
    GdxTri2Phaser() : on(gdx3ds_prof_active != 0 && gdx_tri2_census_on != 0) {
        if (on) {
            child = gdx3ds_prof_child_ticks;
            t = (uint64_t)gdx3ds_prof_now();
        }
    }
    inline void lap(int phase) {
        if (on) {
            const uint64_t now = (uint64_t)gdx3ds_prof_now();
            const uint64_t childNow = gdx3ds_prof_child_ticks;
            gdx_tri2_phase_ticks[phase] += (now - t) - (childNow - child);
            t = now;
            child = childNow;
        }
    }
    inline void count(int c) {
        if (on) {
            gdx_tri2_cnt[c]++;
        }
    }
};
} // namespace
#define GDX_TRI2_BEGIN() GdxTri2Phaser gdxTri2
#define GDX_TRI2_LAP(ph) gdxTri2.lap(kGdxTri2Ph##ph)
#define GDX_TRI2_CNT(c) gdxTri2.count(kGdxTri2Cnt##c)
#else
#define GDX_TRI2_BEGIN() \
    do {                 \
    } while (0)
#define GDX_TRI2_LAP(ph) \
    do {                 \
    } while (0)
#define GDX_TRI2_CNT(c) \
    do {                \
    } while (0)
#endif

// [trifast] LOCKED-60 Task C lever. Killswitch: 3DS `[debug] trifast=0` (latched per gfx task
// in SpReset, default ON), desktop env GDX_TRIFAST=1 (default OFF). Off = the pre-existing
// code paths byte for byte. What it changes, each provably bit-identical in output:
//  1. s7 draw-state memo predicate: the hit test demanded BOTH textures_changed flags clear,
//     but the slow path only clears the flag of a unit the combiner USES — so after any
//     SETTILE/LOAD arm (which arms both), a single-texture material left textures_changed[1]
//     armed forever and every subsequent draw (tri AND rect) missed the memo: the [tri2]
//     census measured memo=0/27527 in a 30-machine race window. A unit the combiner does not
//     use contributes nothing to the banked derivation (its extents stay zero, its import is
//     never run), so its flag is irrelevant to the hit; the flag itself is left armed for the
//     next combiner that does use the unit (that draw misses on the comb key and imports).
//  2. Packed vertex emission ([triloop] path) arithmetic: uint8/255.0f through a 256-entry
//     table of the same expression; UV normalisation by a power-of-two extent as a multiply by
//     the exact reciprocal (division and multiplication by 2^k are both correctly rounded
//     results of the same real number); the shifts>0..10 divide by 1<<shifts likewise;
//     newlib's fminf (two __fpclassifyf calls) inlined with its exact NaN policy; the SHADE
//     combiner-input case read straight from the vertex; the per-vertex fog-line note hoisted
//     to one call per tri (same arguments, last write wins, no reader in between).
extern "C" uint32_t gdx_trifast_stat[13];
enum {
    kGdxTfHit = 0,     // memo hits
    kGdxTfHitNew,      // hits the legacy predicate would have refused (tc[unused] armed)
    kGdxTfMissDirty,   // miss reasons, first failing test in legacy order
    kGdxTfMissInvalid,
    kGdxTfMissComb,
    kGdxTfMissTile,
    kGdxTfMissTc0,
    kGdxTfMissTc1,
    kGdxTfUvMul,       // vertex-unit UV normalisations taken as an exact multiply
    kGdxTfUvDiv,       // ... left as a divide (non-pow2 extent)
    kGdxTfFastTris,    // tris emitted through the fast packed loop
    kGdxTfVfyPack,     // trifast=2: fast-loop tris whose 36 floats differ from the legacy loop
    kGdxTfVfyMemo,     // trifast=2: new-predicate hits whose banked extents the slow path disagrees with
    kGdxTfCount
};
extern "C" uint32_t gdx_trifast_stat[kGdxTfCount] = {};
#ifdef __3DS__
extern "C" int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
static bool sGdxTriFast = true;
// trifast=2: VERIFY mode — the fast paths run and are then shadowed by the legacy computation
// (memo hit re-derived through the slow path, fast packed tri re-emitted by the legacy loop into
// scratch) with every disagreement counted in the receipt. Parity proof independent of game
// nondeterminism; not a performance mode.
static bool sGdxTriFastVerify = false;
static void GdxTriFastRefresh() {
    int mode = 1;
    if (&gdx3ds_config_get_int != nullptr) {
        mode = gdx3ds_config_get_int("debug", "trifast", 1);
    } else if (std::getenv("GDX_TRIFAST_OFF") != nullptr) {
        mode = 0;
    }
    sGdxTriFast = mode != 0;
    sGdxTriFastVerify = mode == 2;
}
extern "C" int gdx_trifast_enabled(void) {
    return sGdxTriFastVerify ? 2 : (sGdxTriFast ? 1 : 0);
}
#else
static const bool sGdxTriFast = std::getenv("GDX_TRIFAST") != nullptr;
static const bool sGdxTriFastVerify = false;
#endif
#ifdef __3DS__
// uint8 -> [0,1] float, bit-identical to `(int)b / 255.0f` (int->float is exact, then one
// correctly rounded divide — the table holds exactly those results).
static const struct GdxByteToUnitTable {
    float v[256];
    GdxByteToUnitTable() {
        for (int k = 0; k < 256; k++) {
            v[k] = k / 255.0f;
        }
    }
} sGdxByteToUnit;
// newlib s_fminf.c verbatim semantics: NaN x -> y, NaN y -> x, else the smaller.
static inline float GdxFminExact(float x, float y) {
    if (x != x) {
        return y;
    }
    if (y != y) {
        return x;
    }
    return x < y ? x : y;
}
#endif
// [tmem2] LOADBLOCK/LOADTILE store-path phase timers + census (LOCKED-60 Task B: F3 costs
// ~12 us/call on hardware, 144-199 calls per crowd frame). The tick accumulators run only
// under the latched gputrace gate (gdx3ds_prof_active, one int test per site when off);
// the counters are plain increments. Both are drained on the [race-dl] cadence by the port
// bridge ([tmem2] line). Ticks are svcGetSystemTick units, so per-phase numbers include one
// timer read each — the bridge prints the measured per-read overhead next to them.
// Killswitch mirror: [debug] tmemfast (3DS, latched per gfx task by the bridge) or the
// GDX_TMEMFAST env (desktop). 1 = fast path, 0 = the byte-identical legacy path.
enum {
    kTmem2PhDerive = 0, // LOADBLOCK: byte counts + the render-tile stride scan + metadata copy
    kTmem2PhPath = 1,   // resource path -> masked-texture lookup
    kTmem2PhCmp = 2,    // GdxTmemSameContentLoad
    kTmem2PhMirror = 3, // TMEM mirror memcpy
    kTmem2PhWalk = 4,   // StoreLoadedTexture: overlap invalidation walk
    kTmem2PhRecord = 5, // StoreLoadedTexture: span record + base entry copy
    kTmem2PhNote = 6,   // GdxNoteTmemLoadKind + dirty arms
    kTmem2PhTotal = 7   // handler entry -> exit (excludes the local's destructor)
};
enum {
    kTmem2StLbCalls = 0, kTmem2StLbSkips = 1, kTmem2StLtCalls = 2, kTmem2StLtSkips = 3,
    kTmem2StResource = 4, kTmem2StRaw = 5, kTmem2StWords = 6, kTmem2StMirrorBytes = 7,
    kTmem2StWalkIters = 8, kTmem2StWalkRelease = 9, kTmem2StWalkSpans = 10,
    kTmem2StFastHit = 11, kTmem2StFastMiss = 12,
    kTmem2StMissInterior = 13, kTmem2StMissDivergent = 14, kTmem2StMissList = 15
};
extern "C" {
uint64_t gdx_tmem2_ph_ticks[8];
uint32_t gdx_tmem2_stat[16];
}
static int GdxTmem2FastInitFromEnv() {
    const char* e = std::getenv("GDX_TMEMFAST");
    return (e != nullptr && e[0] == '0') ? 0 : 1;
}
extern "C" int gdx_tmem2_fast_enabled = GdxTmem2FastInitFromEnv();
#ifdef __3DS__
static inline bool GdxTmem2ProfOn() {
    return gdx3ds_prof_active != 0;
}
static inline uint64_t GdxTmem2Now(bool on) {
    return on ? static_cast<uint64_t>(gdx3ds_prof_now()) : 0u;
}
#else
static inline bool GdxTmem2ProfOn() {
    return false;
}
static inline uint64_t GdxTmem2Now(bool) {
    return 0u;
}
#endif
static inline void GdxTmem2Acc(bool on, int phase, uint64_t t0, uint64_t t1) {
    if (on) {
        gdx_tmem2_ph_ticks[phase] += t1 - t0;
    }
}

// Workshop texture-dump hooks (port/gdx_workshop.cpp).
extern "C" int gdx_workshop_texture_dump_enabled(void);
extern "C" void gdx_workshop_dump_texture(const void* origSrcAddr, size_t origSrcLen,
                                          const char* resourcePathOrNull, const uint8_t* rgba32,
                                          int width, int height, int n64Fmt, int n64Siz);

void Interpreter::GdxDumpDecodedRgba32(int tile, const uint8_t* rgba32, uint32_t width, uint32_t height) {
    if (!gdx_workshop_texture_dump_enabled()) {
        return;
    }
    uint32_t tmem = mRdp->texture_tile[tile].tmem_index;
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(tmem).raw_tex_metadata;
    const void* origAddr = mRdp->LoadedTextureAt(tmem).addr;
    size_t origLen = mRdp->LoadedTextureAt(tmem).size_bytes;
    const char* path = (metadata->resource != nullptr) ? metadata->resource->GetInitData()->Path.c_str() : nullptr;
    gdx_workshop_dump_texture(origAddr, origLen, path, rgba32, (int)width, (int)height,
                              (int)mRdp->texture_tile[tile].fmt, (int)mRdp->texture_tile[tile].siz);
}

void Interpreter::ShaderCacheClear() {
    mRapi->ClearShaderCache();
}

/* Texture-bind signature for testing whether replaying one tick's display list is idempotent:
   interpolation replays the same retained command buffer M times, and StoreLoadedTexture is
   path-dependent, so replay 2 starts from replay 1's end state. */
static uint64_t sGdxTexBindHash = 1469598103934665603ull;

extern "C" void gdx_gfx_texbind_hash_reset(void) {
    sGdxTexBindHash = 1469598103934665603ull;
}

extern "C" unsigned long long gdx_gfx_texbind_hash(void) {
    return (unsigned long long) sGdxTexBindHash;
}

/* Hash the cache key, not texture_id: ids are recycled through free_texture_ids after LRU
   eviction, so identical content can return under a different id. Off by default -- this runs on
   every bind in the hottest loop, replayed M times per tick. */
static const bool sGdxTexBindHashEnabled = [] {
    const char* e = std::getenv("GDX_DIAG_TEXBIND");
    return e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
}();

static inline void GdxNoteTexBind(int slot, const TextureCacheKey& key) {
    if (!sGdxTexBindHashEnabled) {
        return;
    }
    const auto mix = [](uint64_t h, uint64_t v) {
        h ^= v;
        return h * 1099511628211ull;
    };
    sGdxTexBindHash = mix(sGdxTexBindHash, (uint64_t) slot);
    sGdxTexBindHash = mix(sGdxTexBindHash, (uint64_t) (uintptr_t) key.texture_addr);
    sGdxTexBindHash = mix(sGdxTexBindHash, (uint64_t) (uintptr_t) key.palette_addrs[0]);
    sGdxTexBindHash = mix(sGdxTexBindHash, (uint64_t) (uintptr_t) key.palette_addrs[1]);
    sGdxTexBindHash = mix(sGdxTexBindHash, ((uint64_t) key.fmt << 8) | key.siz);
    sGdxTexBindHash = mix(sGdxTexBindHash, (uint64_t) key.palette_index);
    sGdxTexBindHash = mix(sGdxTexBindHash, (uint64_t) key.size_bytes);
    sGdxTexBindHash = mix(sGdxTexBindHash, (uint64_t) key.line_size_bytes);
    sGdxTexBindHash = mix(sGdxTexBindHash, ((uint64_t) key.tile_width << 16) | key.tile_height);
}

/* No separate "miss" hash: hit and miss for the same content must contribute
   identically, or a cold-cache pass diverges from a warm one by construction. */

bool Interpreter::TextureCacheLookup(int i, const TextureCacheKey& key) {
#ifdef __3DS__
    GdxTexCacheNoteThread(0);
#endif
    TextureCacheMap::iterator it = mTextureCache.map.find(key);
    TextureCacheNode** n = &mRenderingState.mTextures[i];

    if (it != mTextureCache.map.end()) {
        if (!it->second.uploaded) {
            // A previous decode reserved the slot but bailed before UploadTexture ran, so
            // texture_id has no GPU resource behind it. Treat as a miss and reuse the reserved
            // id rather than binding an empty texture forever.
            static const bool sDiagCiLatch = std::getenv("GDX_DIAG_CI_LATCH") != nullptr;
            if (sDiagCiLatch) {
                static int sNeverUploadedLogs = 0;
                if (sNeverUploadedLogs < 64) {
                    ++sNeverUploadedLogs;
                    SPDLOG_WARN("[ci-latch] refusing never-uploaded cache entry");
                }
            }
            GdxNoteTexBind(i, key);
            mRapi->SelectTexture(i, it->second.texture_id);
            *n = &*it;
            mRenderingState.sampler_valid[i] = true;
            mRenderingState.sampler_linear_filter[i] = false;
            mRenderingState.sampler_cms[i] = 0;
            mRenderingState.sampler_cmt[i] = 0;
            mTextureCache.lru.splice(mTextureCache.lru.end(), mTextureCache.lru,
                                     it->second.lru_location); // move to back
            return false;
        }

        GdxNoteTexBind(i, key);
        mRapi->SelectTexture(i, it->second.texture_id);
        *n = &*it;
        mGeometryDiagnostics.rgba16OpaquePixels += it->second.rgba16_opaque_pixels;
        mGeometryDiagnostics.rgba16TransparentPixels += it->second.rgba16_transparent_pixels;
        mGeometryDiagnostics.rgba16ForcedOpaquePixels += it->second.rgba16_forced_opaque_pixels;
        mTextureCache.lru.splice(mTextureCache.lru.end(), mTextureCache.lru,
                                 it->second.lru_location); // move to back
        return true;
    }

    if (mTextureCache.map.size() >= TEXTURE_CACHE_MAX_SIZE) {
        // Remove the texture that was least recently used
        it = mTextureCache.lru.front().it;
        mTextureCache.free_texture_ids.push_back(it->second.texture_id);
        for (int j = 0; j < SHADER_MAX_TEXTURES; j++) {
            if (mRenderingState.mTextures[j] == &*it) {
                mRenderingState.mTextures[j] = nullptr;
                // [crowd2] EVICTION INVARIANT: nulling a live binding must re-arm the
                // import pipeline itself — the draw path only re-imports under
                // textures_changed, and the unconditional SETTILE re-arm that used to
                // rescue this hole is now value-gated (redundant re-assertions no
                // longer arm). Both units: j may be a mask/replacement slot whose
                // refresh rides its base unit's flag.
                mRdp->textures_changed[0] = true;
                mRdp->textures_changed[1] = true;
            }
        }
        mTextureCache.map.erase(it);
        mTextureCache.lru.pop_front();
    }

    uint32_t texture_id;
    if (!mTextureCache.free_texture_ids.empty()) {
        texture_id = mTextureCache.free_texture_ids.back();
        mTextureCache.free_texture_ids.pop_back();
    } else {
        texture_id = mRapi->NewTexture();
    }

    it = mTextureCache.map.insert(std::make_pair(key, TextureCacheValue())).first;
    TextureCacheNode* node = &*it;
    node->second.texture_id = texture_id;
    node->second.lru_location = mTextureCache.lru.insert(mTextureCache.lru.end(), { it });

    GdxNoteTexBind(i, key);
    mRapi->SelectTexture(i, texture_id);
#ifdef __3DS__
    /* Seed the fresh slot with the TILE's clamp modes rather than WRAP/WRAP. The decode that
     * follows this miss calls UploadTexture immediately, and the 3DS backend has to know at
     * that moment whether an axis is G_TX_CLAMP so it can replicate the edge texel into the
     * pow2 padding (N64 clamp semantics; see [debug] padgutter in gfx_citro3d.cpp). With the
     * old 0,0 seed the backend saw WRAP on every first upload and padded with transparent
     * black, which GPU_LINEAR then faded the content edge into.
     * Coherent with the draw-time sampler cache: the draw path re-asserts whenever its own
     * (possibly CLAMP-stripped) cms/cmt differ from what is recorded here, and when they
     * agree the hardware already carries exactly these modes. key.cms/key.cmt are part of
     * the cache key, so a tile that wants different modes gets a different entry. */
    const uint8_t seedCms = key.cms;
    const uint8_t seedCmt = key.cmt;
#else
    const uint8_t seedCms = 0;
    const uint8_t seedCmt = 0;
#endif
    mRapi->SetSamplerParameters(i, false, seedCms, seedCmt);
    mRenderingState.sampler_valid[i] = true;
    mRenderingState.sampler_linear_filter[i] = false;
    mRenderingState.sampler_cms[i] = seedCms;
    mRenderingState.sampler_cmt[i] = seedCmt;
    *n = node;
    return false;
}

std::string_view Interpreter::GetBaseTexturePath(std::string_view path) {
    if (path.starts_with(Ship::IResource::gAltAssetPrefix)) {
        return path.substr(Ship::IResource::gAltAssetPrefix.length());
    }

    return path;
}

void Interpreter::TextureCacheDelete(const uint8_t* origAddr) {
#ifdef __3DS__
    GdxTexCacheNoteThread(1);
#endif
    GfxS7InvalidateDrawMemos(); // [perf-s7] may null mRenderingState.mTextures / re-arm imports
#ifdef __3DS__
    unsigned gdxDeleted = 0;
#endif
    while (mTextureCache.map.bucket_count() > 0) {
        TextureCacheKey key = { origAddr, { 0 }, 0, 0, 0 }; // bucket index only depends on the address
        size_t bucket = mTextureCache.map.bucket(key);
        bool again = false;
        for (auto it = mTextureCache.map.begin(bucket); it != mTextureCache.map.end(bucket); ++it) {
            if (it->first.texture_addr == origAddr) {
                for (int j = 0; j < SHADER_MAX_TEXTURES; j++) {
                    if (mRenderingState.mTextures[j] == &*it) {
                        mRenderingState.mTextures[j] = nullptr;
                        // [crowd2] EVICTION INVARIANT (see TextureCacheLookup): re-arm imports.
                        mRdp->textures_changed[0] = true;
                        mRdp->textures_changed[1] = true;
                    }
                }
                mTextureCache.lru.erase(it->second.lru_location);
                mTextureCache.free_texture_ids.push_back(it->second.texture_id);
                mTextureCache.map.erase(it->first);
                again = true;
#ifdef __3DS__
                ++gdxDeleted;
#endif
                break;
            }
        }
        if (!again) {
            break;
        }
    }
#ifdef __3DS__
    if (gdxDeleted != 0) {
        /* The bucket walk above appends in hash-chain order (several tiles of one source
         * address share a bucket), so canonicalise here too: the recycled id must be
         * address-independent WITHIN a screen, not only across the next TextureCacheClear. */
        GdxCanonicalizeFreeTextureIds(mTextureCache.free_texture_ids);
        gdx3ds_texcache_note_delete(gdxDeleted);
    }
#endif

    // The refreshed buffer may also be a TLUT, and palette-keyed entries hash to unrelated
    // buckets. Gated on mSeenPaletteAddrs, so a plain refresh costs one hash lookup.
    TextureCacheDeletePalette(origAddr);
}

// Erase every cache entry whose CI palette key names `paletteAddr`.
//
// CI decodes are keyed on the palette's DRAM address, which cannot see an in-place rewrite of its
// content: the night-course flashing windows restage the same TLUT slot every frame, and
// TextureCacheDelete only matched key.texture_addr. Invalidation rather than content-keying --
// folding palette content into the key (GDX_CI_PALETTE_HASH) froze menu fades.
void Interpreter::TextureCacheDeletePalette(const uint8_t* paletteAddr) {
    if (paletteAddr == nullptr || mSeenPaletteAddrs.find(paletteAddr) == mSeenPaletteAddrs.end()) {
        return;
    }
#ifdef __3DS__
    GdxTexCacheNoteThread(2);
#endif
    GfxS7InvalidateDrawMemos(); // [perf-s7] same rationale as TextureCacheDelete

    static const bool sDiagPaletteEvict = std::getenv("GDX_DIAG_PALETTE_EVICT") != nullptr;
    size_t evicted = 0;

    for (auto it = mTextureCache.map.begin(); it != mTextureCache.map.end();) {
        if (it->first.palette_addrs[0] != paletteAddr && it->first.palette_addrs[1] != paletteAddr) {
            ++it;
            continue;
        }
        for (int j = 0; j < SHADER_MAX_TEXTURES; j++) {
            if (mRenderingState.mTextures[j] == &*it) {
                mRenderingState.mTextures[j] = nullptr;
                // [crowd2] EVICTION INVARIANT (see TextureCacheLookup): re-arm imports.
                mRdp->textures_changed[0] = true;
                mRdp->textures_changed[1] = true;
            }
        }
        mTextureCache.lru.erase(it->second.lru_location);
        mTextureCache.free_texture_ids.push_back(it->second.texture_id);
        it = mTextureCache.map.erase(it);
        ++evicted;
    }

#ifdef __3DS__
    if (evicted != 0) {
        /* Whole-map walk == unordered_map order: same canonicalisation as TextureCacheDelete. */
        GdxCanonicalizeFreeTextureIds(mTextureCache.free_texture_ids);
        gdx3ds_texcache_note_delete((unsigned)evicted);
    }
#endif

    if (sDiagPaletteEvict && evicted != 0) {
        static int sPaletteEvictLogs = 0;
        if (sPaletteEvictLogs < 200) {
            ++sPaletteEvictLogs;
            SPDLOG_WARN("[pal-evict] palette={} evicted={} cache={}", fmt::ptr(paletteAddr), evicted,
                        mTextureCache.map.size());
        }
    }
}

// Shared ordering counter for the GDX_DIAG_SETTILE / GDX_DIAG_UVPROBE traces so
// tile-descriptor writes and draw markers interleave in one timeline.
int gGdxTraceSeq = 0;
// Set by the port bridge when a race venue is loaded; menu screens would
// otherwise exhaust the probe caps before a race is reached.
extern "C" int gGdxRaceActive = 0;
// Ring buffer of recent tile writes; flushed to disk only when a suspect draw
// fires, so the trace shows exactly the SETTILE history leading to that draw.
#include <deque>
std::deque<std::string> gGdxTileTraceRing;
void GdxTileTracePush(const char* line) {
    gGdxTileTraceRing.emplace_back(line);
    if (gGdxTileTraceRing.size() > 512) {
        gGdxTileTraceRing.pop_front();
    }
}

// Pick the per-line byte width for texture decode. Prefer the DRAM stride from
// loaded_texture when it looks like real per-line info (differs from total size).
// Fall back to the TMEM tile stride when loaded sizes match total (LoadBlock with
// width=1, where line_size == full_image_line_size == size).
static uint32_t GetEffectiveLineSize(uint32_t lineSizeBytes, uint32_t fullImageLineSizeBytes, uint32_t sizeBytes,
                                     uint32_t tileLineSizeBytes) {
    if ((lineSizeBytes != sizeBytes || fullImageLineSizeBytes != sizeBytes) && lineSizeBytes > 0) {
        return lineSizeBytes;
    }
    return tileLineSizeBytes;
}

// Readable-extent probe for TMEM mirror copies: bounds a source read to the
// committed memory region so a lying transfer size can never fault.
static size_t TmemSourceReadableLimit(const uint8_t* addr) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const bool readable = (mbi.State == MEM_COMMIT) &&
                              (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (!readable) {
            return 0;
        }
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return static_cast<size_t>(regionEnd - reinterpret_cast<uintptr_t>(addr));
    }
#endif
    return SIZE_MAX;
}

// N64 tile masks define the coordinate wrap period independently from the
// amount of source data loaded into TMEM. Uploading the whole backing image
// makes modern samplers wrap at the wrong boundary, which selects adjacent
// mip levels/atlas regions for repeated track and vehicle materials.
static void ApplyTileMaskExtent(const RDP* rdp, int tile, uint32_t& width, uint32_t& height,
                                bool maskAuthoritative = false) {
    const uint8_t masks = rdp->texture_tile[tile].masks;
    const uint8_t maskt = rdp->texture_tile[tile].maskt;
    const uint8_t cms = rdp->texture_tile[tile].cms;
    const uint8_t cmt = rdp->texture_tile[tile].cmt;
    if (masks != G_TX_NOMASK && masks < 31) {
        // On hardware a wrapping axis samples with a period of exactly 1<<mask, regardless of
        // TMEM load bookkeeping. Where the caller can honor that, the mask wins even if the
        // recorded load size implies a smaller extent.
        if (maskAuthoritative && (cms & G_TX_CLAMP) == 0) {
            width = 1u << masks;
        } else {
            width = std::min<uint32_t>(width, 1u << masks);
        }
    }
    if (maskt != G_TX_NOMASK && maskt < 31) {
        if (maskAuthoritative && (cmt & G_TX_CLAMP) == 0) {
            height = 1u << maskt;
        } else {
            height = std::min<uint32_t>(height, 1u << maskt);
        }
    }
}

void Interpreter::ImportTextureRgba16(int textureUnit, int tile, bool importReplacement, bool forceOpaqueAlpha) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureRgba16: null texture address for tile {}", tile);
        return;
    }

    // TMEM-emulation decode path (GDX_NO_TMEM=1 disables it): decode from emulated TMEM using
    // only tile-descriptor state, like hardware, so the result does not depend on per-slot load
    // bookkeeping, which goes stale under heavy per-frame TMEM reuse. OTR resources and HD
    // replacements keep the legacy path; their data never enters TMEM.
    static const bool sTmemDisabled = std::getenv("GDX_NO_TMEM") != nullptr;
    const auto& tmemTile = mRdp->texture_tile[tile];
    if (!sTmemDisabled && !importReplacement && metadata->resource == nullptr && tmemTile.siz == G_IM_SIZ_16b) {
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemTile.tmem_index) * 8u;
        uint32_t lineBytes = tmemTile.line_size_bytes;
        if (tmemByteOffset < sizeof(mRdp->tmem) && lineBytes >= 2) {
            uint32_t width = lineBytes / 2;
            uint32_t height = (sizeof(mRdp->tmem) - tmemByteOffset) / lineBytes;
            ApplyTileMaskExtent(mRdp, tile, width, height, /*maskAuthoritative=*/true);
            // Clamp axes bound to the declared tile window, like the hardware.
            const uint32_t tileW = static_cast<uint32_t>((tmemTile.lrs - tmemTile.uls + 4) / 4);
            const uint32_t tileH = static_cast<uint32_t>((tmemTile.lrt - tmemTile.ult + 4) / 4);
            // Height/width default to (remaining TMEM / lineBytes), which for a tall atlas
            // (e.g. the 12x160 RGBA16 speedometer digit strip) overshoots the loaded image by
            // reading whatever else streamed through TMEM below it. When an axis carries no mask
            // (G_TX_NOMASK), hardware samples exactly the gDPSetTileSize window (lrs/lrt), so that
            // window is the authoritative extent whether or not the CLAMP bit is set -- without
            // it the decoded height inflates, padH/vScale shift, and every UV-indexed glyph
            // samples the wrong row (garbled speed digits / km-h / tall symbol strips).
            const bool clampS = (tmemTile.cms & G_TX_CLAMP) != 0;
            const bool clampT = (tmemTile.cmt & G_TX_CLAMP) != 0;
            const bool boundW = clampS || tmemTile.masks == G_TX_NOMASK;
            const bool boundH = clampT || tmemTile.maskt == G_TX_NOMASK;
            if (boundW && tileW > 0 && tileW < width) {
                width = tileW;
            }
            if (boundH && tileH > 0 && tileH < height) {
                height = tileH;
            }
            if (width == 0) {
                width = 1;
            }
            if (height == 0) {
                height = 1;
            }
            if (lineBytes * height > sizeof(mRdp->tmem) - tmemByteOffset) {
                height = (sizeof(mRdp->tmem) - tmemByteOffset) / lineBytes;
            }
            if (width > 0 && height > 0) {
                const uint8_t* tmemSrc = mRdp->tmem + tmemByteOffset;
                uint32_t px = 0;
                uint64_t opaqueCount = 0;
                uint64_t transparentCount = 0;
                uint64_t forcedOpaqueCount = 0;
                height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
                for (uint32_t y = 0; y < height; y++) {
                    const uint8_t* row = tmemSrc + static_cast<size_t>(y) * lineBytes;
                    for (uint32_t x = 0; x < width; x++) {
                        const uint16_t col16 = (row[2 * x] << 8) | row[2 * x + 1];
                        uint8_t a = col16 & 1;
                        if (a != 0) {
                            opaqueCount++;
                        } else {
                            transparentCount++;
                            if (forceOpaqueAlpha) {
                                a = 1;
                                forcedOpaqueCount++;
                            }
                        }
                        mTexUploadBuffer[4 * px + 0] = SCALE_5_8(col16 >> 11);
                        mTexUploadBuffer[4 * px + 1] = SCALE_5_8((col16 >> 6) & 0x1f);
                        mTexUploadBuffer[4 * px + 2] = SCALE_5_8((col16 >> 1) & 0x1f);
                        mTexUploadBuffer[4 * px + 3] = a ? 255 : 0;
                        px++;
                    }
                }
                mGeometryDiagnostics.rgba16OpaquePixels += opaqueCount;
                mGeometryDiagnostics.rgba16TransparentPixels += transparentCount;
                mGeometryDiagnostics.rgba16ForcedOpaquePixels += forcedOpaqueCount;
                if (textureUnit >= 0 && textureUnit < SHADER_MAX_TEXTURES &&
                    mRenderingState.mTextures[textureUnit] != nullptr) {
                    TextureCacheValue& cacheValue = mRenderingState.mTextures[textureUnit]->second;
                    cacheValue.rgba16_opaque_pixels = opaqueCount;
                    cacheValue.rgba16_transparent_pixels = transparentCount;
                    cacheValue.rgba16_forced_opaque_pixels = forcedOpaqueCount;
                }
                // A/B: flatten wrapping road-sized textures to gray (also honored
                // here so the diagnostic keeps working on the TMEM decode path).
                static const bool sTmemSolidRoad = std::getenv("GDX_DIAG_SOLIDROAD") != nullptr;
                if (sTmemSolidRoad && width >= 16 && height >= 16 && tmemTile.masks > 0 && tmemTile.maskt > 0) {
                    for (uint32_t p2 = 0; p2 < width * height; ++p2) {
                        mTexUploadBuffer[4 * p2 + 0] = 128;
                        mTexUploadBuffer[4 * p2 + 1] = 128;
                        mTexUploadBuffer[4 * p2 + 2] = 128;
                        mTexUploadBuffer[4 * p2 + 3] = 255;
                    }
                }
                // GDX_DIAG_TMEMCHK=1: log final decode geometry plus a TMEM-vs-DRAM
                // compare, to tell a bad load mirror from bad decode geometry.
                static const bool sTmemChk = std::getenv("GDX_DIAG_TMEMCHK") != nullptr;
                if (sTmemChk && gGdxRaceActive != 0) {
                    static int sTmemChkCount = 0;
                    if (sTmemChkCount < 400) {
                        const auto& slot = mRdp->LoadedTextureAt(tmemTile.tmem_index);
                        uint32_t tmemRowSum = 0;
                        for (uint32_t b = 0; b < lineBytes && b < 256; b++) {
                            tmemRowSum = tmemRowSum * 31u + tmemSrc[b];
                        }
                        int srcCmp = -1; /* -1 = source unavailable */
                        const uint8_t* dram = reinterpret_cast<const uint8_t*>(slot.addr);
                        if (dram != nullptr && slot.size_bytes != 0) {
                            const size_t span = std::min<size_t>(
                                slot.size_bytes, sizeof(mRdp->tmem) - tmemByteOffset);
                            srcCmp = (std::memcmp(tmemSrc, dram, span) == 0) ? 1 : 0;
                        }
                        ++sTmemChkCount;
                        FILE* tf = fopen("tmemchk-trace.txt", sTmemChkCount == 1 ? "w" : "a");
                        if (tf != nullptr) {
                            fprintf(tf,
                                    "D tile=%u tmem=%u line=%u w=%u h=%u masks=%u maskt=%u "
                                    "cms=%u cmt=%u tw=%u th=%u slotsz=%u slotline=%u slotfull=%u "
                                    "rowsum=%08X tmem_eq_dram=%d gen=%u\n",
                                    tile, tmemTile.tmem_index, lineBytes, width, height,
                                    tmemTile.masks, tmemTile.maskt, tmemTile.cms, tmemTile.cmt,
                                    tileW, tileH, slot.size_bytes, slot.line_size_bytes,
                                    slot.full_image_line_size_bytes, tmemRowSum, srcCmp,
                                    mRdp->tmem_generation);
                            fclose(tf);
                        }
                    }
                }
                GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
                mRapi->UploadTexture(mTexUploadBuffer, width, height);
                return;
            }
        }
    }

    uint32_t sizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t line_size_bytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(line_size_bytes, fullImageLineSizeBytes, sizeBytes,
                                               mRdp->texture_tile[tile].line_size_bytes);
    uint32_t width = widthBytes / 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;

    // Preserve the source row stride before reducing the logical upload extent.
    if (fullImageLineSizeBytes == 0 || fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = width * 2;
    }

    // Clamp to the rendered region only when the loaded buffer is ~1.33x of it (mipmap
    // pyramid signature). Window-scrolling tiles have loaded ≈ rendered or loaded >> rendered;
    // skip both. CLAMP wrap mode always opts in.
    uint32_t tile_w = (uint32_t)((mRdp->texture_tile[tile].lrs - mRdp->texture_tile[tile].uls + 4) / 4);
    uint32_t tile_h = (uint32_t)((mRdp->texture_tile[tile].lrt - mRdp->texture_tile[tile].ult + 4) / 4);
    uint32_t loadedPixels = width * height;
    uint32_t renderedPixels = tile_w * tile_h;
    bool pyramidLike =
        renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13; // < 1.625x
    bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
    bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) {
        width = tile_w;
    }
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) {
        height = tile_h;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height, /*maskAuthoritative=*/true);

    // Bound the decode by memory actually readable at `addr` rather than by the recorded load
    // size: slot bookkeeping can understate a load while DRAM still holds the full wrap period.
    const uint32_t rowStrideBytes = (fullImageLineSizeBytes > 0) ? fullImageLineSizeBytes : (width * 2);
    uint32_t readableBytes = (rowStrideBytes > 0 && height > 0) ? rowStrideBytes * height : sizeBytes;
#ifdef _WIN32
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            const bool readable = (mbi.State == MEM_COMMIT) &&
                                  (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                                  PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                  PAGE_EXECUTE_WRITECOPY)) != 0;
            if (!readable) {
                readableBytes = 0;
            } else {
                const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                const uintptr_t avail = regionEnd - reinterpret_cast<uintptr_t>(addr);
                if (avail < readableBytes) {
                    readableBytes = static_cast<uint32_t>(avail);
                }
            }
        }
    }
#endif
    if (rowStrideBytes > 0) {
        const uint32_t maxRows = readableBytes / rowStrideBytes;
        if (height > maxRows) {
            height = maxRows;
        }
    }
    const uint32_t maxTexel = readableBytes / 2;

    uint32_t i = 0;
    uint64_t opaquePixels = 0;
    uint64_t transparentPixels = 0;
    uint64_t forcedOpaquePixels = 0;

    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t clrIdx = (y * (fullImageLineSizeBytes / 2)) + (x);

            uint16_t col16 = (clrIdx < maxTexel) ? ((addr[2 * clrIdx] << 8) | addr[2 * clrIdx + 1]) : 0;
            uint8_t a = col16 & 1;
            if (a != 0) {
                opaquePixels++;
            } else {
                transparentPixels++;
                if (forceOpaqueAlpha) {
                    a = 1;
                    forcedOpaquePixels++;
                }
            }
            uint8_t r = col16 >> 11;
            uint8_t g = (col16 >> 6) & 0x1f;
            uint8_t b = (col16 >> 1) & 0x1f;
            mTexUploadBuffer[4 * i + 0] = SCALE_5_8(r);
            mTexUploadBuffer[4 * i + 1] = SCALE_5_8(g);
            mTexUploadBuffer[4 * i + 2] = SCALE_5_8(b);
            mTexUploadBuffer[4 * i + 3] = a ? 255 : 0;

            i++;
        }
    }

    mGeometryDiagnostics.rgba16OpaquePixels += opaquePixels;
    mGeometryDiagnostics.rgba16TransparentPixels += transparentPixels;
    mGeometryDiagnostics.rgba16ForcedOpaquePixels += forcedOpaquePixels;
    if (textureUnit >= 0 && textureUnit < SHADER_MAX_TEXTURES &&
        mRenderingState.mTextures[textureUnit] != nullptr) {
        TextureCacheValue& cacheValue = mRenderingState.mTextures[textureUnit]->second;
        cacheValue.rgba16_opaque_pixels = opaquePixels;
        cacheValue.rgba16_transparent_pixels = transparentPixels;
        cacheValue.rgba16_forced_opaque_pixels = forcedOpaquePixels;
    }

    // A/B: flatten wrapping road-sized textures to gray, to tell whether the track stripes come
    // from the texture or from the geometry/UVs.
    static const bool diagSolidRoad = std::getenv("GDX_DIAG_SOLIDROAD") != nullptr;
    if (diagSolidRoad && width >= 16 && height >= 16 &&
        mRdp->texture_tile[tile].masks > 0 && mRdp->texture_tile[tile].maskt > 0) {
        for (uint32_t p = 0; p < width * height; ++p) {
            mTexUploadBuffer[4 * p + 0] = 128;
            mTexUploadBuffer[4 * p + 1] = 128;
            mTexUploadBuffer[4 * p + 2] = 128;
            mTexUploadBuffer[4 * p + 3] = 255;
        }
    }

    // Dump road-like RGBA16 uploads as PPM images so the actual decoded texture
    // can be inspected directly (coherent road tile vs garbage vs wrong content).
    static const bool diagRgba16 = std::getenv("GDX_DIAG_RGBA16") != nullptr;
    if (diagRgba16 && width > 0 && height > 0) {
        const bool isRoadLike = (height >= 1 && height <= 64 && width >= 16 && width <= 96) &&
                                mRdp->texture_tile[tile].masks > 0 && mRdp->texture_tile[tile].maskt > 0;
        static int sRgba16DumpCount = 0;
        if (isRoadLike && sRgba16DumpCount < 48) {
            char name[128];
            snprintf(name, sizeof(name), "roadtex_%02d_t%u_%ux%u.ppm", sRgba16DumpCount, tile, width, height);
            ++sRgba16DumpCount;
            FILE* f = fopen(name, "wb");
            if (f != nullptr) {
                fprintf(f, "P6\n%u %u\n255\n", width, height);
                for (uint32_t p = 0; p < width * height; ++p) {
                    fputc(mTexUploadBuffer[4 * p + 0], f);
                    fputc(mTexUploadBuffer[4 * p + 1], f);
                    fputc(mTexUploadBuffer[4 * p + 2], f);
                }
                fclose(f);
            }
            FILE* idx = fopen("roadtex-index.txt", "a");
            if (idx != nullptr) {
                const auto& loadedEntry = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index);
                fprintf(idx,
                        "%s fullLine=%u expLine=%u strideOK=%d cms=%u cmt=%u masks=%u maskt=%u scaleS=%04X "
                        "ldLine=%u ldFull=%u ldSize=%u ldOrig=%u tileLine=%u tmemIdx=%u addr=%p\n",
                        name, fullImageLineSizeBytes, width * 2,
                        (fullImageLineSizeBytes == width * 2) ? 1 : 0,
                        mRdp->texture_tile[tile].cms, mRdp->texture_tile[tile].cmt,
                        mRdp->texture_tile[tile].masks, mRdp->texture_tile[tile].maskt,
                        (unsigned)mRsp->texture_scaling_factor.s,
                        loadedEntry.line_size_bytes, loadedEntry.full_image_line_size_bytes,
                        loadedEntry.size_bytes, loadedEntry.orig_size_bytes,
                        mRdp->texture_tile[tile].line_size_bytes,
                        (unsigned)mRdp->texture_tile[tile].tmem_index,
                        (const void*)addr);
                fclose(idx);
            }
        }
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureRgba32(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureRgba32: null texture address for tile {}", tile);
        return;
    }

    uint32_t size_bytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t full_image_line_size_bytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t line_size_bytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(line_size_bytes, full_image_line_size_bytes, size_bytes,
                                               mRdp->texture_tile[tile].line_size_bytes * 2);
    uint32_t width = widthBytes / 4;
    uint32_t height = widthBytes > 0 ? size_bytes / widthBytes : 0;

    if (full_image_line_size_bytes == 0 || full_image_line_size_bytes == size_bytes) {
        full_image_line_size_bytes = width * 4;
    }

    // Clamp to the rendered region only when the loaded buffer is ~1.33x of it (mipmap
    // pyramid signature). Window-scrolling tiles have loaded ≈ rendered or loaded >> rendered;
    // skip both. CLAMP wrap mode always opts in.
    uint32_t tile_w = (uint32_t)((mRdp->texture_tile[tile].lrs - mRdp->texture_tile[tile].uls + 4) / 4);
    uint32_t tile_h = (uint32_t)((mRdp->texture_tile[tile].lrt - mRdp->texture_tile[tile].ult + 4) / 4);
    uint32_t loadedPixels = width * height;
    uint32_t renderedPixels = tile_w * tile_h;
    bool pyramidLike = renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13;
    bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
    bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) {
        width = tile_w;
    }
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) {
        height = tile_h;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height);

    // Copy pixel by pixel, respecting full image stride (handles sub-tile loads)
    uint32_t fullImageStridePixels = full_image_line_size_bytes / 4;
    uint32_t i = 0;
    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcIdx = y * fullImageStridePixels + x;
            mTexUploadBuffer[4 * i + 0] = addr[4 * srcIdx + 0];
            mTexUploadBuffer[4 * i + 1] = addr[4 * srcIdx + 1];
            mTexUploadBuffer[4 * i + 2] = addr[4 * srcIdx + 2];
            mTexUploadBuffer[4 * i + 3] = addr[4 * srcIdx + 3];
            i++;
        }
    }
    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureIA4(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureIA4: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                               mRdp->texture_tile[tile].line_size_bytes);
    uint32_t width = widthBytes * 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;

    if (fullImageLineSizeBytes == 0 || fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = widthBytes;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height);

    uint32_t i = 0;
    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcPixelIdx = y * (fullImageLineSizeBytes * 2) + x;
            uint8_t byte = addr[srcPixelIdx / 2];
            uint8_t part = (byte >> (4 - (srcPixelIdx % 2) * 4)) & 0xf;
            uint8_t intensity = part >> 1;
            uint8_t alpha = part & 1;
            mTexUploadBuffer[4 * i + 0] = SCALE_3_8(intensity);
            mTexUploadBuffer[4 * i + 1] = SCALE_3_8(intensity);
            mTexUploadBuffer[4 * i + 2] = SCALE_3_8(intensity);
            mTexUploadBuffer[4 * i + 3] = alpha ? 255 : 0;
            i++;
        }
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureIA8(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureIA8: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    uint32_t width = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                          mRdp->texture_tile[tile].line_size_bytes);
    uint32_t height = width > 0 ? sizeBytes / width : 0;

    if (fullImageLineSizeBytes == 0 || fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = width;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height);

    uint32_t i = 0;
    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcIdx = y * fullImageLineSizeBytes + x;
            uint8_t intensity = addr[srcIdx] >> 4;
            uint8_t alpha = addr[srcIdx] & 0xf;
            mTexUploadBuffer[4 * i + 0] = SCALE_4_8(intensity);
            mTexUploadBuffer[4 * i + 1] = SCALE_4_8(intensity);
            mTexUploadBuffer[4 * i + 2] = SCALE_4_8(intensity);
            mTexUploadBuffer[4 * i + 3] = SCALE_4_8(alpha);
            i++;
        }
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureIA16(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureIA16: null texture address for tile {}", tile);
        return;
    }

    uint32_t size_bytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t full_image_line_size_bytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t line_size_bytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(line_size_bytes, full_image_line_size_bytes, size_bytes,
                                               mRdp->texture_tile[tile].line_size_bytes);
    uint32_t width = widthBytes / 2;
    uint32_t height = widthBytes > 0 ? size_bytes / widthBytes : 0;

    // A single line of pixels should not equal the entire image (height == 1 non-withstanding)
    if (full_image_line_size_bytes == 0 || full_image_line_size_bytes == size_bytes) {
        full_image_line_size_bytes = width * 2;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height);

    uint32_t i = 0;

    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t clrIdx = (y * (full_image_line_size_bytes / 2)) + (x);

            uint8_t intensity = addr[2 * clrIdx];
            uint8_t alpha = addr[2 * clrIdx + 1];
            uint8_t r = intensity;
            uint8_t g = intensity;
            uint8_t b = intensity;
            mTexUploadBuffer[4 * i + 0] = r;
            mTexUploadBuffer[4 * i + 1] = g;
            mTexUploadBuffer[4 * i + 2] = b;
            mTexUploadBuffer[4 * i + 3] = alpha;

            i++;
        }
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureI4(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureI4: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    uint32_t widthBytes = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                               mRdp->texture_tile[tile].line_size_bytes);
    uint32_t width = widthBytes * 2;
    uint32_t height = widthBytes > 0 ? sizeBytes / widthBytes : 0;

    // A single line of pixels should not equal the entire image (height == 1 non-withstanding)
    if (fullImageLineSizeBytes == 0 || fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = width / 2;
    }

    // Padded I4 assets can carry a wider source row than the logical render tile. Keep the
    // source stride, but crop the upload to the tile, or UV normalization stretches the padding
    // across the glyph and the text reads as narrow and over-spaced.
    const uint32_t tileWidth =
        static_cast<uint32_t>((mRdp->texture_tile[tile].lrs - mRdp->texture_tile[tile].uls + 4) / 4);
    const uint32_t tileHeight =
        static_cast<uint32_t>((mRdp->texture_tile[tile].lrt - mRdp->texture_tile[tile].ult + 4) / 4);
    // TextureUtils uses WRAP even for FONT_SET_4, so clamp bits cannot identify the visible
    // sub-image. The tile bounds are the authoritative upload extent.
    if (tileWidth > 0 && tileWidth < width) {
        width = tileWidth;
    }
    if (tileHeight > 0 && tileHeight < height) {
        height = tileHeight;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height);

    // A render tile whose implied extent exceeds the recorded load walks past the backing buffer
    // (seen as a heap access violation mid-race). Clamp the read to the loaded byte count:
    // bounded garbage plus a reported identity beats a crash.
    if (sizeBytes != 0 && width > 0 && height > 0) {
        const uint64_t lastIdx =
            static_cast<uint64_t>(height - 1) * (fullImageLineSizeBytes * 2) + (width - 1);
        const uint64_t neededBytes = lastIdx / 2 + 1;
        if (neededBytes > sizeBytes) {
            static int sI4ClampLogs = 0;
            if (sI4ClampLogs < 16) {
                ++sI4ClampLogs;
                SPDLOG_ERROR("ImportTextureI4 CLAMP: tile {} extent {}x{} stride {} needs {}B > "
                             "loaded {}B (tmem {} addr {})",
                             tile, width, height, fullImageLineSizeBytes, neededBytes, sizeBytes,
                             mRdp->texture_tile[tile].tmem_index, fmt::ptr(addr));
            }
            const uint32_t rowBytes = fullImageLineSizeBytes != 0 ? fullImageLineSizeBytes : 1u;
            uint32_t maxRows = static_cast<uint32_t>(sizeBytes / rowBytes);
            if (maxRows == 0) {
                maxRows = 1;
            }
            if (height > maxRows) {
                height = maxRows;
            }
        }
    }

    uint32_t i = 0;

    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t clrIdx = (y * (fullImageLineSizeBytes * 2)) + (x);

            uint8_t byte = addr[clrIdx / 2];
            uint8_t part = (byte >> (4 - (clrIdx % 2) * 4)) & 0xf;
            uint8_t intensity = part;
            uint8_t r = intensity;
            uint8_t g = intensity;
            uint8_t b = intensity;
            uint8_t a = intensity;
            mTexUploadBuffer[4 * i + 0] = SCALE_4_8(r);
            mTexUploadBuffer[4 * i + 1] = SCALE_4_8(g);
            mTexUploadBuffer[4 * i + 2] = SCALE_4_8(b);
            mTexUploadBuffer[4 * i + 3] = SCALE_4_8(a);

            i++;
        }
    }

    /* Checksum the decoded RGBA before upload: zero means the decode produced blackness,
       nonzero-but-invisible convicts the upload/bind layer. */
    {
        static const bool sDiagFontMachine = std::getenv("GDX_DIAG_FONT_MACHINE") != nullptr;
        if (sDiagFontMachine) {
            static int sI4ProbeLogs = 0;
            if (sI4ProbeLogs < 32) {
                ++sI4ProbeLogs;
                uint32_t sum = 0;
                const size_t n = (size_t)width * height * 4;
                for (size_t k = 0; k < n; k++) {
                    sum = sum * 31 + mTexUploadBuffer[k];
                }
                gdx_dbg_logf("[fontmach] I4 decode tile=%d tmem=0x%X w=%u h=%u sum=%08X\n",
                             tile, mRdp->texture_tile[tile].tmem_index, width, height, sum);
                gdx_dbg_logf("[fontmach] I4 in: sizeB=%u origB=%u lineB=%u fullB=%u tileLineB=%u "
                             "tileWH=%ux%u mask=%u/%u cm=%u/%u addr=%p\n",
                             mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes,
                             mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).orig_size_bytes,
                             lineSizeBytes, fullImageLineSizeBytes, mRdp->texture_tile[tile].line_size_bytes,
                             tileWidth, tileHeight, mRdp->texture_tile[tile].masks, mRdp->texture_tile[tile].maskt,
                             mRdp->texture_tile[tile].cms, mRdp->texture_tile[tile].cmt,
                             static_cast<const void*>(addr));
            }
        }
    }
    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureI8(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureI8: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    uint32_t width = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                          mRdp->texture_tile[tile].line_size_bytes);
    uint32_t height = width > 0 ? sizeBytes / width : 0;

    if (fullImageLineSizeBytes == 0 || fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = width;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height);

    uint32_t i = 0;
    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t intensity = addr[y * fullImageLineSizeBytes + x];
            mTexUploadBuffer[4 * i + 0] = intensity;
            mTexUploadBuffer[4 * i + 1] = intensity;
            mTexUploadBuffer[4 * i + 2] = intensity;
            mTexUploadBuffer[4 * i + 3] = intensity;
            i++;
        }
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureCi4(int tile, bool importReplacement) {
    uint32_t fullImageLineSizeBytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureCi4: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t lineSizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;
    uint32_t palIdx = mRdp->texture_tile[tile].palette; // 0-15

    const uint8_t* palette;

    if (mRdp->palettes[palIdx / 8] == nullptr) {
        SPDLOG_WARN("CI4: null palette slot {} for palIdx={}", palIdx / 8, palIdx);
        return;
    }
    palette = mRdp->palettes[palIdx / 8] + (palIdx % 8) * 16 * 2;

    uint32_t baseLineSizeBytes = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                                      mRdp->texture_tile[tile].line_size_bytes);
    uint32_t resultLineSizeBytes = baseLineSizeBytes;

    if (metadata->h_byte_scale != 1) {
        resultLineSizeBytes *= metadata->h_byte_scale;
    }

    // CI4: 2 pixels per byte
    uint32_t width = resultLineSizeBytes * 2;
    uint32_t height = resultLineSizeBytes > 0 ? sizeBytes / resultLineSizeBytes : 0;

    if (fullImageLineSizeBytes == 0 || fullImageLineSizeBytes == sizeBytes) {
        fullImageLineSizeBytes = resultLineSizeBytes;
    }

    // Clamp to the rendered region only when the loaded buffer is ~1.33x of it (mipmap
    // pyramid signature). Window-scrolling tiles have loaded ≈ rendered or loaded >> rendered;
    // skip both. CLAMP wrap mode always opts in.
    uint32_t tile_w = (uint32_t)((mRdp->texture_tile[tile].lrs - mRdp->texture_tile[tile].uls + 4) / 4);
    uint32_t tile_h = (uint32_t)((mRdp->texture_tile[tile].lrt - mRdp->texture_tile[tile].ult + 4) / 4);
    uint32_t loadedPixels = width * height;
    uint32_t renderedPixels = tile_w * tile_h;
    bool pyramidLike = renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13;
    bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
    bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) {
        width = tile_w;
    }
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) {
        height = tile_h;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height);

    uint32_t i = 0;
    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t srcPixelIdx = y * (fullImageLineSizeBytes * 2) + x;
            uint8_t byte = addr[srcPixelIdx / 2];
            uint8_t idx = (byte >> (4 - (srcPixelIdx % 2) * 4)) & 0xf;
            uint16_t col16 = (palette[idx * 2] << 8) | palette[idx * 2 + 1]; // Big endian load
            uint8_t a = col16 & 1;
            uint8_t r = col16 >> 11;
            uint8_t g = (col16 >> 6) & 0x1f;
            uint8_t b = (col16 >> 1) & 0x1f;
            mTexUploadBuffer[4 * i + 0] = SCALE_5_8(r);
            mTexUploadBuffer[4 * i + 1] = SCALE_5_8(g);
            mTexUploadBuffer[4 * i + 2] = SCALE_5_8(b);
            mTexUploadBuffer[4 * i + 3] = a ? 255 : 0;
            i++;
        }
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureCi8(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureCi8: null texture address for tile {}", tile);
        return;
    }

    uint32_t sizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t fullImageLineSizeBytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t lineSizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    if (mRdp->palettes[0] == nullptr || mRdp->palettes[1] == nullptr) {
        SPDLOG_WARN("CI8: null palette (pal0={}, pal1={})", static_cast<const void*>(mRdp->palettes[0]),
                    static_cast<const void*>(mRdp->palettes[1]));
        return;
    }

    uint32_t baseLineSizeBytes = GetEffectiveLineSize(lineSizeBytes, fullImageLineSizeBytes, sizeBytes,
                                                      mRdp->texture_tile[tile].line_size_bytes);
    lineSizeBytes = baseLineSizeBytes;
    if (fullImageLineSizeBytes == 0 || fullImageLineSizeBytes == sizeBytes ||
        fullImageLineSizeBytes < lineSizeBytes) {
        fullImageLineSizeBytes = lineSizeBytes;
    }
    if (lineSizeBytes == 0) {
        SPDLOG_WARN("CI8: zero line size for tile {}", tile);
        return;
    }

    uint32_t resultLineSizeBytes = baseLineSizeBytes;
    if (metadata->h_byte_scale != 1) {
        resultLineSizeBytes *= metadata->h_byte_scale;
    }

    uint32_t width = resultLineSizeBytes;
    uint32_t height = resultLineSizeBytes > 0 ? sizeBytes / resultLineSizeBytes : 0;

    // Clamp to the rendered region only when the loaded buffer is ~1.33x of it (mipmap
    // pyramid signature). Window-scrolling tiles have loaded ≈ rendered or loaded >> rendered;
    // skip both. CLAMP wrap mode always opts in.
    uint32_t tile_w = (uint32_t)((mRdp->texture_tile[tile].lrs - mRdp->texture_tile[tile].uls + 4) / 4);
    uint32_t tile_h = (uint32_t)((mRdp->texture_tile[tile].lrt - mRdp->texture_tile[tile].ult + 4) / 4);
    uint32_t loadedPixels = width * height;
    uint32_t renderedPixels = tile_w * tile_h;
    bool pyramidLike = renderedPixels > 0 && loadedPixels > renderedPixels && loadedPixels * 8 < renderedPixels * 13;
    bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
    bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
    if ((pyramidLike || clampS) && tile_w > 0 && tile_w < width) {
        width = tile_w;
    }
    if ((pyramidLike || clampT) && tile_h > 0 && tile_h < height) {
        height = tile_h;
    }
    ApplyTileMaskExtent(mRdp, tile, width, height);

    uint32_t out = 0;
    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t idx = addr[y * fullImageLineSizeBytes + x];
            uint16_t col16 = (mRdp->palettes[idx / 128][(idx % 128) * 2] << 8) |
                             mRdp->palettes[idx / 128][(idx % 128) * 2 + 1]; // Big endian load
            uint8_t a = col16 & 1;
            uint8_t r = col16 >> 11;
            uint8_t g = (col16 >> 6) & 0x1f;
            uint8_t b = (col16 >> 1) & 0x1f;
            mTexUploadBuffer[4 * out + 0] = SCALE_5_8(r);
            mTexUploadBuffer[4 * out + 1] = SCALE_5_8(g);
            mTexUploadBuffer[4 * out + 2] = SCALE_5_8(b);
            mTexUploadBuffer[4 * out + 3] = a ? 255 : 0;
            ++out;
        }
    }

    // GDX_MINIMAP_PROBE=1: the minimap uploads as CI8 64x32/64x24 halves using only
    // palette indices 0..3. Logs the index histogram and what the loaded TLUT
    // decodes those entries to (index 1 must be opaque black).
    static const bool sMinimapDecodeProbe = std::getenv("GDX_MINIMAP_PROBE") != nullptr;
    if (sMinimapDecodeProbe && width == 64 && height > 0 && height <= 48) {
        uint32_t hist[256] = { 0 };
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                hist[addr[y * fullImageLineSizeBytes + x]]++;
            }
        }
        uint32_t known = hist[0] + hist[1] + hist[2] + hist[3];
        // Skip generic CI8 rects that merely share the 64-wide dimension.
        if (known * 100 >= (uint32_t)(width * height) * 95) {
            auto dec = [&](uint8_t idx, int& r, int& g, int& b, int& a) {
                uint16_t c = (uint16_t)((mRdp->palettes[idx / 128][(idx % 128) * 2] << 8) |
                                        mRdp->palettes[idx / 128][(idx % 128) * 2 + 1]);
                a = c & 1;
                r = c >> 11;
                g = (c >> 6) & 0x1f;
                b = (c >> 1) & 0x1f;
            };
            int r0, g0, b0, a0, r1, g1, b1, a1, r2, g2, b2, a2, r3, g3, b3, a3;
            dec(0, r0, g0, b0, a0);
            dec(1, r1, g1, b1, a1);
            dec(2, r2, g2, b2, a2);
            dec(3, r3, g3, b3, a3);
            static int sMinimapDecodeLogs = 0;
            if (sMinimapDecodeLogs < 24) {
                ++sMinimapDecodeLogs;
                SPDLOG_ERROR("[minimap-ci8] {}x{} src idx: clear0={} black1={} white2={} grey3={} | "
                             "TLUT pal0=({},{},{},a{}) pal1_BLACK=({},{},{},a{}) pal2=({},{},{},a{}) "
                             "pal3=({},{},{},a{})",
                             width, height, hist[0], hist[1], hist[2], hist[3], r0, g0, b0, a0, r1, g1,
                             b1, a1, r2, g2, b2, a2, r3, g3, b3, a3);
            }
        }
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
}

void Interpreter::ImportTextureImg(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureImg: null texture address for tile {}", tile);
        return;
    }

    uint16_t width = metadata->width;
    uint16_t height = metadata->height;
    mRapi->UploadTexture(addr, width, height);
}

void Interpreter::ImportTextureRaw(int tile, bool importReplacement) {
    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* addr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).addr;

    if (addr == nullptr) {
        SPDLOG_ERROR("ImportTextureRaw: null texture address for tile {}", tile);
        return;
    }

    uint16_t width = metadata->width;
    uint16_t height = metadata->height;
    Fast::TextureType type = metadata->type;
    std::shared_ptr<Fast::Texture> resource = metadata->resource;

    // if texture type is CI4 or CI8 we need to apply tlut to it
    switch (type) {
        case Fast::TextureType::Palette4bpp:
            ImportTextureCi4(tile, importReplacement);
            return;
        case Fast::TextureType::Palette8bpp:
            ImportTextureCi8(tile, importReplacement);
            return;
        default:
            break;
    }

    uint32_t numLoadedBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
    uint32_t numOriginallyLoadedBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).orig_size_bytes;

    uint32_t resultOrigLineSize = mRdp->texture_tile[tile].line_size_bytes;
    switch (mRdp->texture_tile[tile].siz) {
        case G_IM_SIZ_32b:
            resultOrigLineSize *= 2;
            break;
    }
    uint32_t resultOrigHeight = numOriginallyLoadedBytes / resultOrigLineSize;
    uint32_t resultNewLineSize = resultOrigLineSize * metadata->h_byte_scale;
    uint32_t resultNewHeight = resultOrigHeight * metadata->v_pixel_scale;

    if (resultNewLineSize == 4 * width && resultNewHeight == height) {
        // Can use the texture directly since it has the correct dimensions
        mRapi->UploadTexture(addr, width, height);
        return;
    }

    uint32_t fullImageLineSizeBytes =
        mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
    uint32_t line_size_bytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;

    // Get the resource's true image size
    uint32_t resourceImageSizeBytes = resource->ImageDataSize;
    uint32_t safeFullImageLineSizeBytes = fullImageLineSizeBytes;
    uint32_t safeLineSizeBytes = line_size_bytes;
    uint32_t safeLoadedBytes = numLoadedBytes;

    // Sometimes the texture load commands will specify a size larger than the authentic texture
    // Normally the OOB info is read as garbage, but will cause a crash on some platforms
    // Restrict the bytes to a safe amount
    if (numLoadedBytes > resourceImageSizeBytes) {
        safeLoadedBytes = resourceImageSizeBytes;
        safeLineSizeBytes = resourceImageSizeBytes;
        safeFullImageLineSizeBytes = resourceImageSizeBytes;
    }

    // C4c: this is the one decoder that fills the staging buffer by byte count rather than
    // by texel, so it takes the byte form of the clamp. Both the copy and the zero-fill are
    // bounded, and the second one starts at an offset the first has already cleared.
    safeLoadedBytes = (uint32_t)gdx3ds_tex_upload_clamp_bytes(safeLoadedBytes);
    numLoadedBytes = (uint32_t)gdx3ds_tex_upload_clamp_bytes(numLoadedBytes);

    // Safely only copy the amount of bytes the resource can allow
    for (uint32_t i = 0, j = 0; i < safeLoadedBytes; i += safeLineSizeBytes, j += safeFullImageLineSizeBytes) {
        memcpy(mTexUploadBuffer + i, addr + j, safeLineSizeBytes);
    }

    // Set the remaining bytes to load as 0
    if (numLoadedBytes > resourceImageSizeBytes) {
        memset(mTexUploadBuffer + resourceImageSizeBytes, 0, numLoadedBytes - resourceImageSizeBytes);
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, resultNewLineSize / 4, resultNewHeight);
    mRapi->UploadTexture(mTexUploadBuffer, resultNewLineSize / 4, resultNewHeight);
}

void Interpreter::ImportTexture(int i, int tile, bool importReplacement) {
    GDX_PROF_SCOPE(IMP); // [prof] lookup + (on miss) full decode
#ifdef __3DS__
    gdx3ds_texcache_note_import();
#endif
    uint8_t fmt = mRdp->texture_tile[tile].fmt;
    uint8_t siz = mRdp->texture_tile[tile].siz;
    uint32_t texFlags = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).tex_flags;
    uint32_t tmemIdex = mRdp->texture_tile[tile].tmem_index;
    uint8_t paletteIndex = mRdp->texture_tile[tile].palette;
    uint32_t origSizeBytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).orig_size_bytes;

    const RawTexMetadata* metadata = &mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).raw_tex_metadata;
    const uint8_t* origAddr =
        importReplacement && (metadata->resource != nullptr)
            ? mMaskedTextures.find(GetBaseTexturePath(metadata->resource->GetInitData()->Path))->second.replacementData
            : mRdp->LoadedTextureAt(tmemIdex).addr;

    // Lookup-side TMEM trace: pairs with StoreLoadedTexture's store log so one
    // run shows whether this tile's tmem base selected the load that populated
    // it (same env gate as the SETTIMG race trace).
    {
        // Effect tiles 1-4 get their own gate (GDX_DIAG_EFFECTTILE) and budget so
        // this probe cannot be starved by env ordering or the shared log caps.
        static const bool sDiagEffectTile = std::getenv("GDX_DIAG_EFFECTTILE") != nullptr;
        if (sDiagEffectTile && gGdxRaceActive != 0 && tile >= 1 && tile <= 4) {
            static int sEffectTileLogs = 0;
            if (sEffectTileLogs < 64) {
                ++sEffectTileLogs;
                SPDLOG_ERROR("[tmem] EFFECT-TILE lookup tile={} tmem={} fmt={} siz={} -> addr={} sizeB={}",
                             tile, tmemIdex, fmt, siz, fmt::ptr(origAddr),
                             mRdp->LoadedTextureAt(tmemIdex).size_bytes);
            }
        }
        if (gGdxRaceActive != 0) { // race-gated, matching the store log
            // Race-first latch, matching StoreLoadedTexture: _putenv precedes race
            // activation, and the old live getenv ran on every import (~100-200/frame).
            static const bool sTmemTrace = std::getenv("GDX_DIAG_SETTIMG") != nullptr;
            if (sTmemTrace) {
                static int sTmemLookupLogs = 0;
                if (sTmemLookupLogs < 400) {
                    ++sTmemLookupLogs;
                    SPDLOG_ERROR("[tmem] lookup tile={} tmem={} fmt={} siz={} -> addr={} sizeB={}", tile,
                                 tmemIdex, fmt, siz, fmt::ptr(origAddr),
                                 mRdp->LoadedTextureAt(tmemIdex).size_bytes);
                }
            }
        }
    }

    // Check if this texture address is a registered GPU framebuffer mirror.
    // If so, bind the GPU FB directly — full resolution, no CPU readback needed.
    if (origAddr != nullptr && !importReplacement) {
        auto fbIt = mFbTextures.find((uintptr_t)origAddr);
        if (fbIt != mFbTextures.end()) {
            Flush();
            mRapi->SelectTextureFb(fbIt->second);
            mRdp->textures_changed[i] = false;
            return;
        }
    }

    if (origAddr == nullptr) {
        static std::array<uint32_t, 32> sNullTextureWarningKeys{};
        static size_t sNullTextureWarningKeyCount = 0;
        const uint32_t warningKey = (static_cast<uint32_t>(tile) << 16) | static_cast<uint16_t>(tmemIdex);
        const auto warningKeysEnd = sNullTextureWarningKeys.begin() + sNullTextureWarningKeyCount;
        if (std::find(sNullTextureWarningKeys.begin(), warningKeysEnd, warningKey) == warningKeysEnd &&
            sNullTextureWarningKeyCount < sNullTextureWarningKeys.size()) {
            sNullTextureWarningKeys[sNullTextureWarningKeyCount++] = warningKey;
            SPDLOG_WARN("ImportTexture: null texture address for tile {} at TMEM word {}; repeats suppressed", tile,
                        tmemIdex);
        }
        return;
    }

    // CI palette-slot guard, hoisted before TextureCacheLookup. mRdp->palettes[] goes transiently
    // null during the port's segment-7 epoch swap at race entry. The old in-decoder check ran
    // after the lookup had already inserted an entry and reserved a texture_id that UploadTexture
    // never filled, so later frames served that empty resource forever. Restricted to siz 4b/8b:
    // CI+16b/32b dispatch to the RGBA importers, which never touch palettes[].
    if (fmt == G_IM_FMT_CI && (siz == G_IM_SIZ_4b || siz == G_IM_SIZ_8b)) {
        bool paletteMissing;
        uint32_t diagPalSlot;
        if (siz == G_IM_SIZ_4b) {
            // Mirrors ImportTextureCi4's derivation: one 16-entry palette; paletteIndex
            // (0-15) selects the staging half via paletteIndex/8.
            diagPalSlot = paletteIndex / 8;
            paletteMissing = mRdp->palettes[diagPalSlot] == nullptr;
        } else {
            // Mirrors ImportTextureCi8's check: CI8 indexes across both staging halves
            // (idx/128 for a 0-255 index), so both halves must be present regardless of
            // paletteIndex.
            diagPalSlot = 0;
            paletteMissing = mRdp->palettes[0] == nullptr || mRdp->palettes[1] == nullptr;
        }
        if (paletteMissing) {
            static const bool sDiagCiLatch = std::getenv("GDX_DIAG_CI_LATCH") != nullptr;
            if (sDiagCiLatch) {
                static int sCiLatchLogs = 0;
                if (sCiLatchLogs < 64) {
                    ++sCiLatchLogs;
                    SPDLOG_WARN("[ci-latch] null palette slot={} palIdx={} addr={} -- decode deferred", diagPalSlot,
                                paletteIndex, fmt::ptr(origAddr));
                }
            }
            return;
        }
    }

    // Use palette_dram_addr (the original DRAM source) instead of palettes[]
    // (which always points to the staging buffer) so the same texture drawn
    // with different palettes gets distinct cache entries.
    TextureCacheKey key;
    if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            uint8_t palSlot = paletteIndex / 8;
            key = { origAddr,
                    { palSlot == 0 ? mRdp->palette_dram_addr[0] : nullptr,
                      palSlot == 1 ? mRdp->palette_dram_addr[1] : nullptr },
                    fmt,
                    siz,
                    paletteIndex,
                    origSizeBytes };
        } else {
            // CI8 uses both palette halves
            key = { origAddr,     { mRdp->palette_dram_addr[0], mRdp->palette_dram_addr[1] }, fmt, siz, paletteIndex,
                    origSizeBytes };
        }
    } else {
        key = { origAddr, {}, fmt, siz, paletteIndex, origSizeBytes };
    }
    key.line_size_bytes = mRdp->LoadedTextureAt(tmemIdex).line_size_bytes;
    key.full_image_line_size_bytes = mRdp->LoadedTextureAt(tmemIdex).full_image_line_size_bytes;

    // CI palette-content hash ([native] palhash, or GDX_CI_PALETTE_HASH on desktop). The key
    // carries the palette's DRAM address, which cannot see an in-place fade rewriting content at
    // that same address, so the fade returned a stale decode and froze. The RDP re-reads the TLUT
    // for every primitive, so the palette's CONTENT is part of the texture's identity. Hashes the
    // fixed 256-byte staging buffers, never raw DRAM, so it cannot fault. The 3DS lever is read
    // through the same latch the owned interpreter uses, so BOTH caches key CI the same way and
    // interp=1 stays comparable with the stock path.
    static const bool sCiPaletteHashEnv = std::getenv("GDX_CI_PALETTE_HASH") != nullptr;
    const bool sCiPaletteHash =
        sCiPaletteHashEnv || (&gdx_ninterp_palhash != nullptr && gdx_ninterp_palhash() != 0);
    if (sCiPaletteHash && fmt == G_IM_FMT_CI) {
        uint32_t palHash = 2166136261u;
        const auto foldPalette = [&palHash](const uint8_t* pal) {
            for (uint32_t b = 0; b < 256u; b++) {
                palHash = (palHash ^ pal[b]) * 16777619u;
            }
        };
        if (siz == G_IM_SIZ_4b) {
            // CI4 uses one 16-entry palette; paletteIndex/8 selects the staging half
            // the key's palette_addrs already references.
            foldPalette(mRdp->palette_staging[(paletteIndex / 8) & 1]);
        } else {
            // CI8 spans both palette halves.
            foldPalette(mRdp->palette_staging[0]);
            foldPalette(mRdp->palette_staging[1]);
        }
        key.palette_content_hash = palHash;
    }
    const uint32_t cacheTileWidth =
        mRdp->texture_tile[tile].lrs >= mRdp->texture_tile[tile].uls
            ? (mRdp->texture_tile[tile].lrs - mRdp->texture_tile[tile].uls + 4u) / 4u
            : 0u;
    const uint32_t cacheTileHeight =
        mRdp->texture_tile[tile].lrt >= mRdp->texture_tile[tile].ult
            ? (mRdp->texture_tile[tile].lrt - mRdp->texture_tile[tile].ult + 4u) / 4u
            : 0u;
    key.tile_width = static_cast<uint16_t>(std::min<uint32_t>(cacheTileWidth, 0xFFFFu));
    key.tile_height = static_cast<uint16_t>(std::min<uint32_t>(cacheTileHeight, 0xFFFFu));
    key.cms = mRdp->texture_tile[tile].cms;
    key.cmt = mRdp->texture_tile[tile].cmt;
    key.masks = mRdp->texture_tile[tile].masks;
    key.maskt = mRdp->texture_tile[tile].maskt;
    static const bool diagnosticForcePreFlxOpaqueAlpha =
        std::getenv("GDX_DIAG_FORCE_PREFLX_OPAQUE_ALPHA") != nullptr;
    const bool forceOpaqueAlpha =
        diagnosticForcePreFlxOpaqueAlpha && mF3dex2Variant != F3dex2Variant::FZeroFlxReject;
    key.force_opaque_alpha = forceOpaqueAlpha;

    // TMEM-decoded textures are cached by content: hash the tile's emulated-TMEM span so the same
    // source address with different TMEM contents cannot hit a stale entry. RGBA16 always; the
    // I/IA font paths (which decode reused arena addresses and served stale glyphs) unless
    // GDX_FONT_CONTENT_HASH=0. CI is excluded -- folding it in here froze menu fades, and its
    // palette aspect is handled opt-in above. line_size_bytes already accounts for 4-bit packing,
    // so min(remaining TMEM, lineBytes * 64) covers any format.
    static const bool sTmemCacheDisabled = std::getenv("GDX_NO_TMEM") != nullptr;
    static const bool sFontContentHashDisabled = [] {
        const char* v = std::getenv("GDX_FONT_CONTENT_HASH");
        return v != nullptr && v[0] == '0';
    }();
    const bool isRgba16 = (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_16b);
    const bool isFontFmt = (fmt == G_IM_FMT_I && (siz == G_IM_SIZ_4b || siz == G_IM_SIZ_8b)) ||
                           (fmt == G_IM_FMT_IA &&
                            (siz == G_IM_SIZ_4b || siz == G_IM_SIZ_8b || siz == G_IM_SIZ_16b));
    const bool hashThisFormat = isRgba16 || (!sFontContentHashDisabled && isFontFmt);
    if (!sTmemCacheDisabled && !importReplacement && metadata->resource == nullptr && hashThisFormat) {
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIdex) * 8u;
        const uint32_t lineBytes = mRdp->texture_tile[tile].line_size_bytes;
        if (tmemByteOffset < sizeof(mRdp->tmem) && lineBytes >= 2) {
            const uint32_t remaining = sizeof(mRdp->tmem) - tmemByteOffset;
            // Span = the bytes the decode actually reads, not a fixed 64-line guess. The old
            // `lineBytes * 64` bound reached to the END of TMEM for common strides, so the key
            // for a stable texture folded in every OTHER texture streaming through TMEM that
            // frame -- each import minted a fresh key (hash never repeated), every import was a
            // cache miss, and race scenes re-decoded + re-swizzled + re-uploaded the same
            // textures every frame (the [c3d] texUp thrash) while flooding the 1024-entry LRU.
            //
            // Default: the recorded load extent (the TMEM copy of the source bytes) -- exactly
            // the content identity the hash exists to capture for the DRAM-decoding font paths.
            uint32_t span = std::min<uint32_t>(remaining, std::max<uint32_t>(
                                                              mRdp->LoadedTextureAt(tmemIdex).size_bytes, 8u));
            if (isRgba16) {
                // The RGBA16 TMEM decode reads rows [0, h) at stride lineBytes, bytes [0, 2*w)
                // per row. Mirror its extent derivation exactly (ImportTextureRgba16): any byte
                // it reads is in the key, so this stays a strict content identity -- and no byte
                // it does NOT read can destabilize the key.
                uint32_t w = lineBytes / 2;
                uint32_t h = remaining / lineBytes;
                ApplyTileMaskExtent(mRdp, tile, w, h, /*maskAuthoritative=*/true);
                // Mirror ImportTextureRgba16's NOMASK bound (the tall-atlas extent fix): on an axis
                // that carries no mask, hardware samples exactly the gDPSetTileSize window, so the
                // decode reads only that window. The hash MUST cover the same bytes and no more --
                // otherwise the span reaches past the loaded atlas into whatever else streamed
                // through the shared low TMEM (e.g. the 12x160 NOMASK+WRAP speedometer digit strip
                // hashed 170 rows while the decode read 160), so the key of a STATIC atlas changed
                // every frame: every import missed, LUS re-decoded and the 3DS backend
                // re-Morton-swizzled + re-uploaded it every frame, and a re-upload race could serve
                // a rotated texture_id -- the residual green/magenta speedometer garbage that
                // survived the decode-side extent fix. Bounding here restores a stable content key.
                const bool clampS = (mRdp->texture_tile[tile].cms & G_TX_CLAMP) != 0;
                const bool clampT = (mRdp->texture_tile[tile].cmt & G_TX_CLAMP) != 0;
                const bool boundW = clampS || mRdp->texture_tile[tile].masks == G_TX_NOMASK;
                const bool boundH = clampT || mRdp->texture_tile[tile].maskt == G_TX_NOMASK;
                if (boundW && cacheTileWidth > 0 && cacheTileWidth < w) {
                    w = cacheTileWidth;
                }
                if (boundH && cacheTileHeight > 0 && cacheTileHeight < h) {
                    h = cacheTileHeight;
                }
                if (w == 0) {
                    w = 1;
                }
                if (h == 0) {
                    h = 1;
                }
                if (lineBytes * h > remaining) {
                    h = remaining / lineBytes;
                }
                if (w > 0 && h > 0) {
                    span = std::min<uint32_t>(remaining, (h - 1) * lineBytes + 2 * w);
                }
            }
            // FNV-1a folded 4 bytes at a time (base is 8-byte aligned; span is measured in
            // TMEM bytes): this runs on EVERY import of a hashed format, ~100/frame in race
            // scenes, so the byte loop was real per-frame CPU.
            uint32_t contentHash = 2166136261u;
            const uint8_t* tmemBytes = mRdp->tmem + tmemByteOffset;
            uint32_t b = 0;
            for (; b + 4 <= span; b += 4) {
                uint32_t word;
                std::memcpy(&word, tmemBytes + b, 4);
                contentHash = (contentHash ^ word) * 16777619u;
            }
            for (; b < span; b++) {
                contentHash = (contentHash ^ tmemBytes[b]) * 16777619u;
            }
            key.tmem_content_hash = contentHash;

            // GDX_DIAG_SPEEDTEX: dump the exact per-import identity of the two F-Zero X speedometer
            // RGBA16 atlases (12x160 aSpeedDigitsTex, 20x16 aKmhTex) so an emulator pass can confirm
            // the content key is now STABLE frame-to-frame (was thrashing pre-fix because the NOMASK
            // hash span over-read past the atlas). A stable h= across frames for one addr means the
            // key no longer aliases; a changing h= for identical decoded content is the bug's tell.
            static const bool sDiagSpeedTex = std::getenv("GDX_DIAG_SPEEDTEX") != nullptr;
            if (sDiagSpeedTex && isRgba16) {
                const uint32_t decodedW = lineBytes / 2;
                const bool isDigitStrip = decodedW == 12 && cacheTileHeight == 160;
                const bool isKmh = decodedW == 20 && cacheTileHeight == 16;
                if (isDigitStrip || isKmh) {
                    static int sSpeedTexLogs = 0;
                    if (sSpeedTexLogs < 240) {
                        ++sSpeedTexLogs;
                        SPDLOG_ERROR("[speedtex] {} addr={} tmem={} line={} tileWxH={}x{} span={} hash={:08x}",
                                     isKmh ? "kmh" : "digits", fmt::ptr(origAddr), tmemIdex, lineBytes,
                                     cacheTileWidth, cacheTileHeight, span, contentHash);
                    }
                }
            }
        }
    }

    // Resource-backed textures (OTR/O2R filepath SETTIMGs) are keyed by their ImageData
    // address, which upstream LUS could treat as immortal because resources were never
    // unloaded. The 3DS resource-cache byte budget (lus-resource-cache-cap.patch) breaks
    // that assumption: an evicted Texture resource frees its ImageData, the next resource
    // load can recycle the SAME heap address for a DIFFERENT texture, and when descriptor
    // geometry also matches (the F-Zero X machine decals are dozens of same-fmt 16x16 I4
    // blocks) the address-keyed lookup HITS the stale entry and binds the WRONG texture's
    // GPU upload -- wrong-content decals with correct shapes. Fold a stable per-resource
    // identity into the otherwise-unused tmem_content_hash so a recycled address can never
    // satisfy another resource's key. Reloads of the SAME resource keep an identical hash
    // (the path is content-stable), so this costs no extra uploads in the steady state.
    // Menus draw their text/UI overwhelmingly through this resource path, so an unstable
    // identity here is per-frame re-decode + re-swizzle + re-upload churn on every screen.
    if (!importReplacement && metadata->resource != nullptr) {
#ifdef __3DS__
        // SELECT-PERF: the o2r-filepath SETTIMG handler precomputes this fold once per
        // resolution (memoized across frames) and ships it in the metadata, so the
        // per-import string walk below only runs for metadata minted by the other
        // SETTIMG paths (path_hash == 0). Value-identical by construction
        // (GdxFoldResourcePathHash mirrors this code), so texture-cache keys are
        // stable across memoized and non-memoized loads.
        if (metadata->path_hash != 0) {
            key.tmem_content_hash = metadata->path_hash;
        } else {
#endif
            const auto initData = metadata->resource->GetInitData();
            // Device receipts on feat/3ds-tilebind ([ship-draw] khash=00000001 on EVERY
            // resource) proved this loader's InitData Id is a constant 1, so keying on it
            // collided every same-geometry resource in the texture cache (rival liveries
            // painted onto the player body). The archive path is the only guaranteed-distinct,
            // content-stable identity: ALWAYS FNV-1a the path; never trust Id.
            uint64_t stableId = 0;
            if (initData != nullptr) {
                uint64_t pathHash = 1469598103934665603ull;
                for (const char c : initData->Path) {
                    pathHash = (pathHash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
                }
                stableId = pathHash;
            }
            key.tmem_content_hash = static_cast<uint32_t>(stableId ^ (stableId >> 32));
            if (key.tmem_content_hash == 0) {
                key.tmem_content_hash = 1; // never collide with the unhashed-key sentinel
            }
#ifdef __3DS__
        }
#endif
    }

    // Zero-sized textures divide by zero or fault UploadTexture. Hoisted before
    // TextureCacheLookup so such a load never mints an uploaded == false entry, which could never
    // be satisfied and re-ran this prologue every frame.
    if (mRdp->texture_tile[tile].line_size_bytes == 0 || mRdp->LoadedTextureAt(tmemIdex).size_bytes == 0 ||
        origAddr == nullptr) {
        return;
    }

#ifdef __3DS__
    // SHIP-LIVERY-2: log the texture id this lookup BINDS for the consuming draw
    // (hit or miss — the id is assigned either way) plus the resource identity, so
    // the port's id→name table can resolve every subsequent draw's bound textures.
    {
        const bool liveryCacheHit = TextureCacheLookup(i, key);
        const TextureCacheNode* liveryNode = mRenderingState.mTextures[i];
        gdx3ds_livery_note_import(i, tile, (unsigned)tmemIdex, fmt, siz,
                                  liveryNode != nullptr ? liveryNode->second.texture_id : 0xffffffffu,
                                  metadata->resource != nullptr
                                      ? metadata->resource->GetInitData()->Path.c_str()
                                      : nullptr,
                                  origAddr, key.tile_width, key.tile_height, liveryCacheHit ? 1 : 0);
        if (liveryCacheHit) {
            return;
        }
    }

    gdx3ds_texcache_note_miss(origAddr, fmt, siz, tmemIdex, key.line_size_bytes,
                              mRdp->LoadedTextureAt(tmemIdex).size_bytes, key.tile_width, key.tile_height,
                              key.tmem_content_hash);
#else
    if (TextureCacheLookup(i, key)) {
        return;
    }
#endif

    if ((texFlags & TEX_FLAG_LOAD_AS_IMG) != 0) {
        ImportTextureImg(tile, importReplacement);
    } else if ((texFlags & TEX_FLAG_LOAD_AS_RAW) != 0) {
        // if load as raw is set then we load_raw();
        ImportTextureRaw(tile, importReplacement);
    } else {
        switch (fmt) {
            case G_IM_FMT_RGBA:
                if (siz == G_IM_SIZ_16b) {
                    ImportTextureRgba16(i, tile, importReplacement, forceOpaqueAlpha);
                } else if (siz == G_IM_SIZ_32b) {
                    ImportTextureRgba32(tile, importReplacement);
                } else {
                    // Rate-limited: no helper runs for this branch, so a persistently
                    // bad dlist would otherwise re-log every frame.
                    static int sRgbaBadSizeLogs = 0;
                    if (sRgbaBadSizeLogs < 8) {
                        ++sRgbaBadSizeLogs;
                        SPDLOG_ERROR("RGBA Texture that isn't 16 or 32 bit. Size = {}", siz);
                    }
                    // OTRTODO: Sometimes, seemingly randomly, we end up here. Could be a bad dlist, could be
                    // something F3D does not have supported. Further investigation is needed.
                }
                break;
            case G_IM_FMT_IA:
                if (siz == G_IM_SIZ_4b) {
                    ImportTextureIA4(tile, importReplacement);
                } else if (siz == G_IM_SIZ_8b) {
                    ImportTextureIA8(tile, importReplacement);
                } else if (siz == G_IM_SIZ_16b) {
                    ImportTextureIA16(tile, importReplacement);
                } else {
                    static int sIaBadSizeLogs = 0;
                    if (sIaBadSizeLogs < 8) {
                        ++sIaBadSizeLogs;
                        SPDLOG_ERROR("IA Texture that isn't 4, 8, or 16 bit. Size = {}", siz);
                    }
                }
                break;
            case G_IM_FMT_CI:
                if (siz == G_IM_SIZ_4b) {
                    ImportTextureCi4(tile, importReplacement);
                } else if (siz == G_IM_SIZ_8b) {
                    ImportTextureCi8(tile, importReplacement);
                } else if (siz == G_IM_SIZ_16b) {
                    // CI+16b is hardware-invalid on N64. The tile's fmt is likely
                    // stale from a prior draw. Decode as RGBA16 instead.
                    ImportTextureRgba16(i, tile, importReplacement, forceOpaqueAlpha);
                } else if (siz == G_IM_SIZ_32b) {
                    ImportTextureRgba32(tile, importReplacement);
                } else {
                    static int sCiBadSizeLogs = 0;
                    if (sCiBadSizeLogs < 8) {
                        ++sCiBadSizeLogs;
                        SPDLOG_ERROR("CI Texture with unexpected size = {}", siz);
                    }
                }
                break;
            case G_IM_FMT_I:
                if (siz == G_IM_SIZ_4b) {
                    ImportTextureI4(tile, importReplacement);
                } else if (siz == G_IM_SIZ_8b) {
                    ImportTextureI8(tile, importReplacement);
                } else {
                    static int sIBadSizeLogs = 0;
                    if (sIBadSizeLogs < 8) {
                        ++sIBadSizeLogs;
                        SPDLOG_ERROR("I Texture that isn't 4 or 8 bit. Size = {}", siz);
                    }
                }
                break;
            case G_IM_FMT_YUV: {
                static int sYuvLogs = 0;
                if (sYuvLogs < 8) {
                    ++sYuvLogs;
                    SPDLOG_ERROR("YUV Textures not supported");
                }
                break;
            }
            default: {
                static int sInvalidFmtLogs = 0;
                if (sInvalidFmtLogs < 8) {
                    ++sInvalidFmtLogs;
                    SPDLOG_ERROR("Invalid texture format. Fmt = {}", fmt);
                }
                break;
            }
        }
    }

    // Mark the entry uploaded even when the dispatch above reached no helper (YUV, unsupported
    // siz/fmt): otherwise the lookup treats it as a permanent miss and the decode re-runs every
    // frame. Safe because the only transient failure, a missing CI palette, returned above before
    // any entry existed.
    if (mRenderingState.mTextures[i] != nullptr) {
        mRenderingState.mTextures[i]->second.uploaded = true;
    }
}

void Interpreter::ImportTextureMask(int i, int tile) {
    uint32_t tmemIndex = mRdp->texture_tile[tile].tmem_index;
    RawTexMetadata metadata = mRdp->LoadedTextureAt(tmemIndex).raw_tex_metadata;

    if (metadata.resource == nullptr) {
        return;
    }

    auto maskIter = mMaskedTextures.find(GetBaseTexturePath(metadata.resource->GetInitData()->Path));
    if (maskIter == mMaskedTextures.end()) {
        return;
    }

    const uint8_t* orig_addr = maskIter->second.mask;

    if (orig_addr == nullptr) {
        return;
    }

    TextureCacheKey key = { orig_addr, {}, 0, 0, 0, 0 };

    if (TextureCacheLookup(i, key)) {
        return;
    }

    uint32_t width = mRdp->texture_tile[tile].line_size_bytes;
    uint32_t height = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).orig_size_bytes /
                      mRdp->texture_tile[tile].line_size_bytes;
    switch (mRdp->texture_tile[tile].siz) {
        case G_IM_SIZ_4b:
            width *= 2;
            break;
        case G_IM_SIZ_8b:
        default:
            break;
        case G_IM_SIZ_16b:
            width /= 2;
            break;
        case G_IM_SIZ_32b:
            width /= 4;
            break;
    }

    height = gdx3ds_tex_upload_clamp_rows(width, height); // C4c: bound the decode to the staging reservation
    for (uint32_t texIndex = 0; texIndex < width * height; texIndex++) {
        uint8_t masked = orig_addr[texIndex];
        if (masked) {
            mTexUploadBuffer[4 * texIndex + 0] = 0;
            mTexUploadBuffer[4 * texIndex + 1] = 0;
            mTexUploadBuffer[4 * texIndex + 2] = 0;
            mTexUploadBuffer[4 * texIndex + 3] = 0xFF;
        } else {
            mTexUploadBuffer[4 * texIndex + 0] = 0;
            mTexUploadBuffer[4 * texIndex + 1] = 0;
            mTexUploadBuffer[4 * texIndex + 2] = 0;
            mTexUploadBuffer[4 * texIndex + 3] = 0;
        }
    }

    GdxDumpDecodedRgba32(tile, mTexUploadBuffer, width, height);
    mRapi->UploadTexture(mTexUploadBuffer, width, height);
    // This path has its own TextureCacheLookup call, outside ImportTexture's
    // uploaded latch, so latch here or the mask re-decodes every frame.
    if (mRenderingState.mTextures[i] != nullptr) {
        mRenderingState.mTextures[i]->second.uploaded = true;
    }
}

void Interpreter::NormalizeVector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

void Interpreter::TransposedMatrixMul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

void Interpreter::MatrixMul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

void Interpreter::CalculateNormalDir(const F3DLight_t* light, float coeffs[3]) {
    float light_dir[3] = { light->dir[0] / 127.0f, light->dir[1] / 127.0f, light->dir[2] / 127.0f };

    Interpreter::TransposedMatrixMul(coeffs, light_dir,
                                     mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1]);
    Interpreter::NormalizeVector(coeffs);
}

void Interpreter::GfxSpMatrix(uint8_t parameters, const int32_t* addr) {
    GDX_PROF_SCOPE(MTX); // [prof] fixed->float convert + stack op + MP recompute
    float matrix[4][4];

    auto it = mCurMtxReplacements->find((Mtx*)addr);
    if (it != mCurMtxReplacements->end()) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                matrix[i][j] = (int)(it->second.mf[i][j] * 65536.0f) * (1.0f / 65536.0f);
            }
        }
    } else {
#ifndef GBI_FLOATS
        // Original GBI where fixed point matrices are used
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j += 2) {
                int32_t int_part = addr[i * 2 + j / 2];
                uint32_t frac_part = addr[8 + i * 2 + j / 2];
                matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
                matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
            }
        }
#else
        // For a modified GBI where fixed point values are replaced with floats
        memcpy(matrix, addr, sizeof(matrix));
#endif
    }

    const int8_t mtx_projection = get_attr(MTX_PROJECTION);
    const int8_t mtx_load = get_attr(MTX_LOAD);
    const int8_t mtx_push = get_attr(MTX_PUSH);

    if (parameters & mtx_projection) {
        if (parameters & mtx_load) {
            memcpy(mRsp->P_matrix, matrix, sizeof(matrix));
        } else {
            MatrixMul(mRsp->P_matrix, matrix, mRsp->P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & mtx_push) && mRsp->modelview_matrix_stack_size < 11) {
            ++mRsp->modelview_matrix_stack_size;
            memcpy(mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1],
                   mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & mtx_load) {
            if (mRsp->modelview_matrix_stack_size == 0)
                ++mRsp->modelview_matrix_stack_size;
            memcpy(mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            MatrixMul(mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], matrix,
                      mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1]);
        }
        mRsp->lights_changed = 1;
    }
    MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1], mRsp->P_matrix);
}

void Interpreter::GfxSpPopMatrix(uint32_t count) {
    while (count--) {
        if (mRsp->modelview_matrix_stack_size > 0) {
            --mRsp->modelview_matrix_stack_size;
            if (mRsp->modelview_matrix_stack_size > 0) {
                MatrixMul(mRsp->MP_matrix, mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1],
                          mRsp->P_matrix);
            }
        }
    }
    mRsp->lights_changed = true;
}

// Runtime fixed-aspect flag, published per frame by the port (input_bridge.c) so EK-editor frames
// render stock 4:3. A plain global, not a CVar: it must never be persisted (a stale saved copy
// pillarboxed the next boot) and it is read on per-frame paths where CVar lookups are too costly.
static int sGdxForceFixedAspect = 0;
extern "C" void gdx_set_force_fixed_aspect(int on) {
    sGdxForceFixedAspect = on ? 1 : 0;
}
extern "C" int gdx_get_force_fixed_aspect(void) {
    return sGdxForceFixedAspect;
}

// Hor+ x-compression factor AdjXForAspectRatio applies to 2D geometry, exposed so the game thread
// can pre-compensate reveal scissors: gDPSetScissor maps linearly across the full frame, but
// gSPTextureRectangle artwork is compressed about screen centre, so a scissor computed in linear
// 320-space clips the panel it should reveal. Reads only per-frame-latched caches, so it is safe
// from the game fiber. Returns 1.0f on 4:3, forced fixed aspect, or widescreen off.
extern "C" float gdx_get_widescreen_geometry_xscale(void) {
    auto gfx = mInstance.lock();
    if (!gfx) {
        return 1.0f;
    }
    if (!gfx->mWidescreenEnabledCache || gfx->mForceFixedAspectCache) {
        return 1.0f;
    }
    if (gfx->mCurDimensions.width == 0 || gfx->mCurDimensions.height == 0) {
        return 1.0f;
    }
    const float aspect = (float)gfx->mCurDimensions.width / (float)gfx->mCurDimensions.height;
    const float xscale = (4.0f / 3.0f) / aspect;
    // The dimension fields are read lock-free from game-logic callers, and a torn read must not
    // leak inf/NaN into callers that cast the result to s32. Floor at 0.15, not 0.25: 48:9
    // triple-monitor yields xscale exactly 0.25.
    if (!(xscale > 0.15f && xscale < 4.0f)) {
        return 1.0f;
    }
    return xscale;
}

// 3DS: extra slack on top of the hor+ widening while the full frame shows (RemoveBorders, the
// 1P widescreen race frame). The stock 4:3 cull popped chunks behind the CRT-safe border; with
// no border the pop lands on the screen edge (gdx3ds_widescreen.h). Weak: 1.0 without the 3DS
// shell (host tests).
extern "C" float gdx3ds_widescreen_cull_margin(void) __attribute__((weak));

static float WidescreenCullMargin(bool bordersRemoved) {
    if (!bordersRemoved || &gdx3ds_widescreen_cull_margin == nullptr) {
        return 1.0f;
    }
    const float margin = gdx3ds_widescreen_cull_margin();
    if (!(margin >= 1.0f && margin <= 2.0f)) {
        return 1.0f;
    }
    return margin;
}

// Widening multiplier for the game's CPU-side frustum tests: it culls against +-1.0 in its own
// 4:3 projection space, but under hor+ the visible band is +-(aspect/(4/3)), so on-screen
// ultrawide content was culled (side pop-in). Returns exactly 1.0 unless UltrawideMode is on and
// the frame renders widescreen, so it is an IEEE-exact no-op in the stock configuration. Separate
// from the geometry xscale above because this one changes culling and stays opt-in.
extern "C" float gdx_get_ultrawide_cull_xscale(void) {
    auto gfx = mInstance.lock();
    if (!gfx || !gfx->mUltrawideCache) {
        return 1.0f;
    }
    const float xscale = gdx_get_widescreen_geometry_xscale();
    if (!(xscale > 0.0f)) {
        return 1.0f;
    }
    return (1.0f / xscale) * WidescreenCullMargin(gfx->mRemoveBordersCache);
}

float Interpreter::AdjXForAspectRatio(float x) const {
    // Skip widescreen adjustment for fixed-size off-screen FBs (HUD elements,
    // small capture buffers), or those which specify a fixed aspect ratio.
    if (mFbActive && mActiveFrameBuffer != mFrameBuffers.end() &&
        (!mActiveFrameBuffer->second.resize || mActiveFrameBuffer->second.forceFixedAspect)) {
        return x;
    } else {
        // Widescreen == 0 skips the hor+ correction so the game renders native 4:3, and
        // Fast3dGui::DrawGame composites into a centred pillarbox. ForceFixedAspect (EK editors)
        // rides the same path. Both are latched per frame in StartFrame, since this runs per
        // vertex where CVar string-hash lookups are too costly.
        if (!mWidescreenEnabledCache || mForceFixedAspectCache) {
            return x;
        }
        return x * (4.0f / 3.0f) / ((float)mCurDimensions.width / (float)mCurDimensions.height);
    }
}

// Scale the width and height value based on the ratio of the viewport to the native size
void Interpreter::AdjustWidthHeightForScale(uint32_t& width, uint32_t& height, uint32_t nativeWidth,
                                            uint32_t nativeHeight) const {
    width = round(width * (mCurDimensions.width / (2.0f * (nativeWidth / 2))));
    height = round(height * (mCurDimensions.height / (2.0f * (nativeHeight / 2))));

    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
}

// [traffic] Shared with GfxSpTri1's clipper: plane 0 of the homogeneous clipper is w >= this.
static constexpr float kGdxMinClipW = 1.0e-5f;

// [traffic] Recompute the per-vertex clip OUTSIDE mask with the clipper's exact plane
// arithmetic (clipDistance(v, plane) < 0, same expressions and order). Every producer of a
// LoadedVertex that can reach the non-rect GfxSpTri1 path must keep this in sync with
// x/y/z/w — GfxSpVertex inlines the same tests, UpdateLoadedVertexClipFlags calls this for
// the clip-fan vertices, and GfxSpLine3DGdx calls it for its screen-space quad.
static inline uint8_t GdxComputeClipOutMask(const LoadedVertex& vertex) {
    uint8_t mask = 0;
    if (vertex.w - kGdxMinClipW < 0.0f) {
        mask |= 1u << 0;
    }
    if (vertex.x + vertex.w < 0.0f) {
        mask |= 1u << 1;
    }
    if (vertex.w - vertex.x < 0.0f) {
        mask |= 1u << 2;
    }
    if (vertex.y + vertex.w < 0.0f) {
        mask |= 1u << 3;
    }
    if (vertex.w - vertex.y < 0.0f) {
        mask |= 1u << 4;
    }
    if (vertex.z + vertex.w < 0.0f) {
        mask |= 1u << 5;
    }
    if (vertex.w - vertex.z < 0.0f) {
        mask |= 1u << 6;
    }
    return mask;
}

void Interpreter::GfxSpVertex(size_t n_vertices, size_t dest_index, const F3DVtx* vertices) {
    GDX_PROF_SCOPE(VTX); // [prof] per-vertex matrix transform + lighting + fog bake
#ifdef __3DS__
    if (gdx3ds_prof_active != 0) {
        gdx3ds_prof_vtx_loaded += (uint32_t)n_vertices; // vtxN column
    }
#endif
    // [perf-dispatch] Hoist MP_matrix out of the per-vertex loop: the stores through
    // d/loaded_vertices may alias mRsp for all the compiler knows, so it re-loaded all 16
    // floats through the member pointer chain on EVERY vertex. Nothing in the loop writes
    // MP_matrix (matrix ops are separate commands); copy it to locals once per G_VTX batch.
    // The diagnostics fingerprint below keeps reading mRsp->MP_matrix — same values, and it
    // documents that the hash captures the live RSP state.
    float mp[4][4];
    memcpy(mp, mRsp->MP_matrix, sizeof(mp));

    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const F3DVtx_t* v = &vertices[i].v;
        const F3DVtx_tn* vn = &vertices[i].n;
        struct LoadedVertex* d = &mRsp->loaded_vertices[dest_index];

        if (v == nullptr) {
            return;
        }

        float x = v->ob[0] * mp[0][0] + v->ob[1] * mp[1][0] + v->ob[2] * mp[2][0] + mp[3][0];
        float y = v->ob[0] * mp[0][1] + v->ob[1] * mp[1][1] + v->ob[2] * mp[2][1] + mp[3][1];
        float z = v->ob[0] * mp[0][2] + v->ob[1] * mp[1][2] + v->ob[2] * mp[2][2] + mp[3][2];
        float w = v->ob[0] * mp[0][3] + v->ob[1] * mp[1][3] + v->ob[2] * mp[2][3] + mp[3][3];

        // Fingerprint the transform, not just its cardinality. Capture MP_matrix on the pass's
        // first vertex so an already-divergent matrix is distinguishable from one that drifts
        // partway through the walk.
        if (sGdxVertexHashEnabled) {
            if (mGeometryDiagnostics.verticesLoaded == 0) {
                for (size_t r = 0; r < 4; ++r) {
                    for (size_t c = 0; c < 4; ++c) {
                        GdxHashFloat(mGeometryDiagnostics.mpFirstHash, mRsp->MP_matrix[r][c]);
                    }
                }
            }
            GdxHashFloat(mGeometryDiagnostics.vertexHash, x);
            GdxHashFloat(mGeometryDiagnostics.vertexHash, y);
            GdxHashFloat(mGeometryDiagnostics.vertexHash, z);
            GdxHashFloat(mGeometryDiagnostics.vertexHash, w);
        }

        mGeometryDiagnostics.verticesLoaded++;
        // [perf-s7] diagnostics-only arithmetic (3 divides/vertex); see gdx_s7_geo_diag_enabled.
        if (gdx_s7_geo_diag_enabled) {
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) {
                mGeometryDiagnostics.invalidVertices++;
            } else {
                if (w <= 0.0f) {
                    mGeometryDiagnostics.verticesNonPositiveW++;
                }
                if (z < -w) {
                    mGeometryDiagnostics.verticesOutsideNear++;
                }
                if (z > w) {
                    mGeometryDiagnostics.verticesOutsideFar++;
                }
                if (fabsf(w) >= 0.000001f) {
                    const float ndcX = x / w;
                    const float ndcY = y / w;
                    const float ndcZ = z / w;
                    mGeometryDiagnostics.minNdcX = std::min(mGeometryDiagnostics.minNdcX, ndcX);
                    mGeometryDiagnostics.maxNdcX = std::max(mGeometryDiagnostics.maxNdcX, ndcX);
                    mGeometryDiagnostics.minNdcY = std::min(mGeometryDiagnostics.minNdcY, ndcY);
                    mGeometryDiagnostics.maxNdcY = std::max(mGeometryDiagnostics.maxNdcY, ndcY);
                    mGeometryDiagnostics.minNdcZ = std::min(mGeometryDiagnostics.minNdcZ, ndcZ);
                    mGeometryDiagnostics.maxNdcZ = std::max(mGeometryDiagnostics.maxNdcZ, ndcZ);
                }
            }
        }

        float world_pos[3] = { 0.0 };
        if (mRsp->geometry_mode & G_LIGHTING_POSITIONAL) {
            float(*mtx)[4] = mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1];
            world_pos[0] = v->ob[0] * mtx[0][0] + v->ob[1] * mtx[1][0] + v->ob[2] * mtx[2][0] + mtx[3][0];
            world_pos[1] = v->ob[0] * mtx[0][1] + v->ob[1] * mtx[1][1] + v->ob[2] * mtx[2][1] + mtx[3][1];
            world_pos[2] = v->ob[0] * mtx[0][2] + v->ob[1] * mtx[1][2] + v->ob[2] * mtx[2][2] + mtx[3][2];
        }

        // Honor G_EX_WIDESCREEN_STRETCH here as well as in GfxDrawRectangle: screen transitions
        // redraw the captured widescreen frame as triangles inside a STRETCH scope, and the plain
        // hor+ path would squeeze that capture into the central 4:3 band. Scaling clip-space x
        // scales NDC x by the same factor, so this is exactly the rectangle path's adjustment.
        // With the flag unset the historical single AdjXForAspectRatio call runs unchanged.
        if ((mRsp->extra_geometry_mode & G_EX_WIDESCREEN_STRETCH) != 0) {
            const bool fixedAspectFramebuffer =
                mFbActive && mActiveFrameBuffer != mFrameBuffers.end() &&
                (!mActiveFrameBuffer->second.resize || mActiveFrameBuffer->second.forceFixedAspect);
            const bool widescreenFrameActive =
                !fixedAspectFramebuffer &&
                ((float)mCurDimensions.width / (float)mCurDimensions.height) > (4.0f / 3.0f) &&
                mWidescreenEnabledCache && !mForceFixedAspectCache;
            if (widescreenFrameActive) {
                // Identical calibration to GfxDrawRectangle: native safe area x=12..308 -> full viewport.
                constexpr float kSafeAreaScale = 160.0f / (160.0f - 12.0f);
                x = x * kSafeAreaScale;
            } else {
                x = AdjXForAspectRatio(x);
            }
        } else {
            x = AdjXForAspectRatio(x);
        }

        short U = v->tc[0] * mRsp->texture_scaling_factor.s >> 16;
        short V = v->tc[1] * mRsp->texture_scaling_factor.t >> 16;
        bool hasF3dflxAlpha = false;
        uint8_t f3dflxAlpha = 0;

        if (mRsp->geometry_mode & G_LIGHTING) {
            if (mRsp->lights_changed) {
                for (int i = 0; i < mRsp->current_num_lights - 1; i++) {
                    CalculateNormalDir(&mRsp->current_lights[i].l, mRsp->current_lights_coeffs[i]);
                }
                /*static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};*/
                CalculateNormalDir(&mRsp->lookat[0], mRsp->current_lookat_coeffs[0]);
                CalculateNormalDir(&mRsp->lookat[1], mRsp->current_lookat_coeffs[1]);
                mRsp->lights_changed = false;
            }

            int r = mRsp->current_lights[mRsp->current_num_lights - 1].l.col[0];
            int g = mRsp->current_lights[mRsp->current_num_lights - 1].l.col[1];
            int b = mRsp->current_lights[mRsp->current_num_lights - 1].l.col[2];

            for (int i = 0; i < mRsp->current_num_lights - 1; i++) {
                float intensity = 0;
                if ((mRsp->geometry_mode & G_LIGHTING_POSITIONAL) && (mRsp->current_lights[i].p.unk3 != 0)) {
                    // Calculate distance from the light to the vertex
                    float dist_vec[3] = { mRsp->current_lights[i].p.pos[0] - world_pos[0],
                                          mRsp->current_lights[i].p.pos[1] - world_pos[1],
                                          mRsp->current_lights[i].p.pos[2] - world_pos[2] };
                    float dist_sq =
                        dist_vec[0] * dist_vec[0] + dist_vec[1] * dist_vec[1] +
                        dist_vec[2] * dist_vec[2] * 2; // The *2 comes from GLideN64, unsure of why it does it
                    float dist = sqrt(dist_sq);

                    // Transform distance vector (which acts as a direction light vector) into model's space
                    float light_model[3];
                    TransposedMatrixMul(light_model, dist_vec,
                                        mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1]);

                    // Calculate intensity for each axis using standard formula for intensity
                    float light_intensity[3];
                    for (int light_i = 0; light_i < 3; light_i++) {
                        light_intensity[light_i] = 4.0f * light_model[light_i] / dist_sq;
                        light_intensity[light_i] = std::clamp(light_intensity[light_i], -1.0f, 1.0f);
                    }

                    // Adjust intensity based on surface normal and sum up total
                    float total_intensity =
                        light_intensity[0] * vn->n[0] + light_intensity[1] * vn->n[1] + light_intensity[2] * vn->n[2];
                    total_intensity = std::clamp(total_intensity, -1.0f, 1.0f);

                    // Attenuate intensity based on attenuation values.
                    // Example formula found at https://ogldev.org/www/tutorial20/tutorial20.html
                    // Specific coefficients for MM's microcode sourced from GLideN64
                    // https://github.com/gonetz/GLideN64/blob/3b43a13a80dfc2eb6357673440b335e54eaa3896/src/gSP.cpp#L636
                    float distf = floorf(dist);
                    float attenuation = (distf * mRsp->current_lights[i].p.unk7 * 2.0f +
                                         distf * distf * mRsp->current_lights[i].p.unkE / 8.0f) /
                                            (float)0xFFFF +
                                        1.0f;
                    intensity = total_intensity / attenuation;
                } else {
                    intensity += vn->n[0] * mRsp->current_lights_coeffs[i][0];
                    intensity += vn->n[1] * mRsp->current_lights_coeffs[i][1];
                    intensity += vn->n[2] * mRsp->current_lights_coeffs[i][2];
                    intensity /= 127.0f;
                }
                if (intensity > 0.0f) {
                    r += intensity * mRsp->current_lights[i].l.col[0];
                    g += intensity * mRsp->current_lights[i].l.col[1];
                    b += intensity * mRsp->current_lights[i].l.col[2];
                }
            }

            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            if (mRsp->geometry_mode & G_TEXTURE_GEN) {
                if (mF3dex2Variant == F3dex2Variant::FZeroFlxReject &&
                    mRsp->f3dflx_alpha_light_valid) {
                    float alphaLight[3];
                    TransposedMatrixMul(alphaLight, mRsp->f3dflx_alpha_light,
                                        mRsp->modelview_matrix_stack[mRsp->modelview_matrix_stack_size - 1]);
                    NormalizeVector(alphaLight);
                    const float intensity = alphaLight[0] * vn->n[0] + alphaLight[1] * vn->n[1] +
                                            alphaLight[2] * vn->n[2];
                    const int lutIndex = std::clamp(static_cast<int>(intensity), -128, 127);
                    const int dmemIndex = static_cast<int>(mRsp->dma_io_dmem) + 128 + lutIndex;
                    if (dmemIndex >= 0 && dmemIndex < static_cast<int>(sizeof(mRsp->dmem))) {
                        f3dflxAlpha = mRsp->dmem[dmemIndex];
                        hasF3dflxAlpha = true;
                        mGeometryDiagnostics.f3dflxAlphaVertices++;
                    }
                } else {
                    float dotx = 0, doty = 0;
                    dotx += vn->n[0] * mRsp->current_lookat_coeffs[0][0];
                    dotx += vn->n[1] * mRsp->current_lookat_coeffs[0][1];
                    dotx += vn->n[2] * mRsp->current_lookat_coeffs[0][2];
                    doty += vn->n[0] * mRsp->current_lookat_coeffs[1][0];
                    doty += vn->n[1] * mRsp->current_lookat_coeffs[1][1];
                    doty += vn->n[2] * mRsp->current_lookat_coeffs[1][2];

                    dotx /= 127.0f;
                    doty /= 127.0f;

                    dotx = Ship::Math::clamp(dotx, -1.0f, 1.0f);
                    doty = Ship::Math::clamp(doty, -1.0f, 1.0f);

                    if (mRsp->geometry_mode & G_TEXTURE_GEN_LINEAR) {
                        dotx = acosf(-dotx) /* M_PI */ * 0.159155f;
                        doty = acosf(-doty) /* M_PI */ * 0.159155f;
                    } else {
                        dotx = (dotx + 1.0f) / 4.0f;
                        doty = (doty + 1.0f) / 4.0f;
                    }

                    U = (int32_t)(dotx * mRsp->texture_scaling_factor.s);
                    V = (int32_t)(doty * mRsp->texture_scaling_factor.t);
                }
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }

        d->u = U;
        d->v = V;

        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) {
            d->clip_rej |= 1; // CLIP_LEFT
        }
        if (x > w) {
            d->clip_rej |= 2; // CLIP_RIGHT
        }
        if (y < -w) {
            d->clip_rej |= 4; // CLIP_BOTTOM
        }
        if (y > w) {
            d->clip_rej |= 8; // CLIP_TOP
        }
        // if (z < -w) d->clip_rej |= 16; // CLIP_NEAR
        if (z > w) {
            d->clip_rej |= 32; // CLIP_FAR
        }

        // [traffic] Clip OUTSIDE mask for GfxSpTri1's pre-scan, using the clipper's exact
        // plane arithmetic (x + w < 0, not x < -w: the two can disagree by one rounding step
        // and the pre-scan must reproduce clipDistance() bit-for-bit). Inlined rather than
        // calling GdxComputeClipOutMask so x/y/z/w stay in registers on this hot loop.
        {
            uint8_t clipOut = 0;
            if (w - kGdxMinClipW < 0.0f) {
                clipOut |= 1u << 0;
            }
            if (x + w < 0.0f) {
                clipOut |= 1u << 1;
            }
            if (w - x < 0.0f) {
                clipOut |= 1u << 2;
            }
            if (y + w < 0.0f) {
                clipOut |= 1u << 3;
            }
            if (w - y < 0.0f) {
                clipOut |= 1u << 4;
            }
            if (z + w < 0.0f) {
                clipOut |= 1u << 5;
            }
            if (w - z < 0.0f) {
                clipOut |= 1u << 6;
            }
            d->clip_out = clipOut;
        }

        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;

        if (mRsp->geometry_mode & G_FOG) {
            /* The RSP computes fog from z/w, which hardware keeps inside [-1, 1] by clipping
               before fog is evaluated. This interpreter renders unclipped near/behind-camera
               vertices to match the reject microcodes, where z/w blows past that domain and
               F-Zero X's steep fog curve saturates whole surfaces. Clamp to the hardware domain;
               a vertex at or behind the camera takes the near (fog = 0) end. */
            float fogRatio;
            if (w <= 0.001f) {
                fogRatio = -1.0f;
            } else {
                fogRatio = Ship::Math::clamp(z / w, -1.0f, 1.0f);
            }

            float fog_z = fogRatio * mRsp->fog_mul + mRsp->fog_offset;
            fog_z = Ship::Math::clamp(fog_z, 0.0f, 255.0f);
            d->color.a = fog_z; // Use alpha variable to store fog factor

            // GDX_DIAG_FOG=1: log per-vertex fog inputs and result during a race.
            static const bool sDiagFog = std::getenv("GDX_DIAG_FOG") != nullptr;
            if (sDiagFog && gGdxRaceActive != 0) {
                static int sFogTraceCount = 0;
                if (sFogTraceCount < 2000) {
                    ++sFogTraceCount;
                    FILE* tf = fopen("fog-trace.txt", sFogTraceCount == 1 ? "w" : "a");
                    if (tf != nullptr) {
                        fprintf(tf, "F fm=%d fo=%d z=%.1f w=%.1f r=%.4f fog=%.1f\n", mRsp->fog_mul,
                                mRsp->fog_offset, z, w, fogRatio, fog_z);
                        fclose(tf);
                    }
                }
            }
        } else if (hasF3dflxAlpha) {
            d->color.a = f3dflxAlpha;
        } else {
            d->color.a = v->cn[3];
        }
    }
}

void Interpreter::GfxSpModifyVertex(uint16_t vtx_idx, uint8_t where, uint32_t val) {
    SUPPORT_CHECK(where == G_MWO_POINT_ST);

    int16_t s = (int16_t)(val >> 16);
    int16_t t = (int16_t)val;

    LoadedVertex* v = &mRsp->loaded_vertices[vtx_idx];
    v->u = s;
    v->v = t;
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
static inline uint32_t alpha_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

static LoadedVertex InterpolateLoadedVertex(const LoadedVertex& a, const LoadedVertex& b, float t) {
    LoadedVertex result{};
    result.x = a.x + (b.x - a.x) * t;
    result.y = a.y + (b.y - a.y) * t;
    result.z = a.z + (b.z - a.z) * t;
    result.w = a.w + (b.w - a.w) * t;
    result.u = a.u + (b.u - a.u) * t;
    result.v = a.v + (b.v - a.v) * t;
    result.color.r = static_cast<uint8_t>(std::clamp(a.color.r + (b.color.r - a.color.r) * t, 0.0f, 255.0f));
    result.color.g = static_cast<uint8_t>(std::clamp(a.color.g + (b.color.g - a.color.g) * t, 0.0f, 255.0f));
    result.color.b = static_cast<uint8_t>(std::clamp(a.color.b + (b.color.b - a.color.b) * t, 0.0f, 255.0f));
    result.color.a = static_cast<uint8_t>(std::clamp(a.color.a + (b.color.a - a.color.a) * t, 0.0f, 255.0f));
    return result;
}

static void UpdateLoadedVertexClipFlags(LoadedVertex& vertex) {
    vertex.clip_out = GdxComputeClipOutMask(vertex);
    vertex.clip_rej = 0;
    if (vertex.x < -vertex.w) {
        vertex.clip_rej |= 1;
    }
    if (vertex.x > vertex.w) {
        vertex.clip_rej |= 2;
    }
    if (vertex.y < -vertex.w) {
        vertex.clip_rej |= 4;
    }
    if (vertex.y > vertex.w) {
        vertex.clip_rej |= 8;
    }
    if (vertex.z < -vertex.w) {
        vertex.clip_rej |= 16;
    }
    if (vertex.z > vertex.w) {
        vertex.clip_rej |= 32;
    }
}

// GDX_RECT_HALF_TEXEL=1 extends the BILERP half-texel offset to texture rectangles; default off.
// It was briefly default-on and reverted: it did not fix the glyph defect it targeted (whose tile
// is CLAMP-addressed, where the supposed texel -1 bleed cannot occur), and rect quads already land
// pixel centres on texel centres, so the extra half texel blurs every BILERP rect in the game.
// Kept behind the switch so the hypothesis stays cheap to re-test.
static const bool sGdxRectHalfTexelDisabled = [] {
    const char* v = std::getenv("GDX_RECT_HALF_TEXEL");
    return !(v != nullptr && v[0] == '1');
}();

#ifdef __3DS__
// [native] track: read-only snapshot of the draw-state memo + packed-batch latches for the
// port's marker replay (gdx3ds_track_interp.h). No state is mutated.
void Interpreter::GdxTrackPackState(::GdxTrackPackState* out) {
    out->packBatch = sGdxPackBatch;
    out->packDst = sGdxPackDst;
    out->params = &sGdxPackParams;
    out->aux = &sGdxPackAux;
    out->memoValid = sGfxS7TriMemo.valid;
    out->memoDirty = (sGfxS7DirtyMask & kGfxS7DirtyTri) != 0;
    out->inputMap = sGfxS7TriMemo.comb != nullptr
                        ? static_cast<const ColorCombiner*>(sGfxS7TriMemo.comb)->shader_input_mapping
                        : nullptr;
    out->prg = sGfxS7TriMemo.prg;
    out->tm = sGfxS7TriMemo.tm;
    for (int i = 0; i < 2; i++) {
        out->texWidth[i] = sGfxS7TriMemo.tex_width[i];
        out->texHeight[i] = sGfxS7TriMemo.tex_height[i];
        out->texWidth2[i] = sGfxS7TriMemo.tex_width2[i];
        out->texHeight2[i] = sGfxS7TriMemo.tex_height2[i];
        out->effectiveTile[i] = sGfxS7TriMemo.effective_tile[i];
        out->usedTextures[i] = sGfxS7TriMemo.usedTextures[i];
    }
    out->clip.zIsFrom0To1 = sGfxS7TriMemo.clip_parameters.z_is_from_0_to_1;
    out->clip.invertY = sGfxS7TriMemo.clip_parameters.invertY;
    out->cullBoth = get_attr(CULL_BOTH);
    out->cullFront = get_attr(CULL_FRONT);
    out->cullBack = get_attr(CULL_BACK);
    out->triFast = sGdxTriFast;
    out->rejectVariant =
        (mF3dex2Variant == F3dex2Variant::Reject || mF3dex2Variant == F3dex2Variant::FZeroFlxReject);
    out->minClipW = kGdxMinClipW;
}

bool gfx_set_combine_handler_rdp(F3DGfx** cmd0);
// [native] track: a template-embedded G_SETCOMBINE replayed exactly as gfx_step would run it —
// the RDP handler plus the draw-state dirty mask the dispatch entry ORs in.
void Interpreter::GdxTrackSetCombine(uint32_t w0, uint32_t w1) {
    F3DGfx cmd;
    cmd.words.w0 = w0;
    cmd.words.w1 = w1;
    F3DGfx* p = &cmd;
    gfx_set_combine_handler_rdp(&p);
    sGfxS7DirtyMask |= static_cast<uint8_t>(kGfxS7DirtyTri | kGfxS7DirtyRect | kGfxS7DirtyRectViewport);
}
#endif

void Interpreter::GfxSpTri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
    // [prof] state resolve + clip + repack into bufVbo. Exclusive time: any flush ->
    // DrawTriangles, ImportTexture, or clip-fan re-entry into this function nested
    // inside this call subtracts itself via the child counter.
    GDX_PROF_SCOPE(TRI);
    GDX_TRI2_BEGIN(); // [tri2] per-phase census (gputrace+verbose only)
    GDX_TRI2_CNT(Calls);
    if (is_rect) {
        GDX_TRI2_CNT(Rect);
    }
#ifdef __3DS__
    if (!is_rect) {
        sTrectTriSeq++; // [trect] a game triangle ends the texrect run
    }
#endif
    // A/B isolation: skip every triangle drawn with the combine mode given in
    // GDX_DIAG_SKIP_COMBINE (hex). Whatever vanishes from the frame identifies
    // the mesh that owns that material.
    static const uint64_t sSkipCombine = [] {
        const char* env = std::getenv("GDX_DIAG_SKIP_COMBINE");
        return (env != nullptr) ? strtoull(env, nullptr, 16) : 0ULL;
    }();
    if (sSkipCombine != 0 && !is_rect && mRdp->combine_mode == sSkipCombine) {
        return;
    }
    // Clipped polygons are triangulated by recursively submitting a fan through
    // this function. Do not clip those generated triangles a second time:
    // floating-point intersections can land infinitesimally outside a plane
    // and otherwise recurse until the stack overflows.
    static thread_local uint32_t sClipFanDepth = 0;
    struct LoadedVertex* v1 = &mRsp->loaded_vertices[vtx1_idx];
    struct LoadedVertex* v2 = &mRsp->loaded_vertices[vtx2_idx];
    struct LoadedVertex* v3 = &mRsp->loaded_vertices[vtx3_idx];
    struct LoadedVertex* v_arr[3] = { v1, v2, v3 };

    // if (rand()%2) return;

    if (!is_rect) {
        mGeometryDiagnostics.trianglesSubmitted++;
    }

    /* [fontmach] probe E: the Create Machine preview renders as uniform ENV color,
       implicating vertex SHADE == 0. Log one line per distinct lighting/shade input
       state so the zeroed source (light colors, ambient, or vertex color) is named. */
    if (!is_rect) {
        static const bool sDiagFontMachineE = std::getenv("GDX_DIAG_FONT_MACHINE") != nullptr;
        if (sDiagFontMachineE) {
            const bool lit = (mRsp->geometry_mode & G_LIGHTING) != 0;
            const int numLights = mRsp->current_num_lights;
            const int ambIdx = numLights > 0 ? numLights - 1 : 0;
            const F3DLight_t& amb = mRsp->current_lights[ambIdx].l;
            const F3DLight_t& l0 = mRsp->current_lights[0].l;
            /* Key on material identity, not per-vertex shade: keying on vertex color
               exhausts the ledger on one mesh's gradient. */
            const uint64_t shadeKey = (static_cast<uint64_t>(lit) << 63) |
                                      (static_cast<uint64_t>(numLights & 0xF) << 58) |
                                      (static_cast<uint64_t>(amb.col[0] & 0xF8) << 47) |
                                      (static_cast<uint64_t>(amb.col[2] & 0xF8) << 42) |
                                      (static_cast<uint64_t>(l0.col[0] & 0xF8) << 37) |
                                      (static_cast<uint64_t>(l0.col[2] & 0xF8) << 32) |
                                      (static_cast<uint64_t>(mRdp->env_color.r) << 24) |
                                      (static_cast<uint64_t>(mRdp->env_color.g) << 16) |
                                      (static_cast<uint64_t>(mRdp->env_color.b) << 8) |
                                      ((mRdp->combine_mode ^ (mRdp->combine_mode >> 32)) & 0xFF);
            static uint64_t sSeenShadeStates[64] = {};
            static int sSeenShadeCount = 0;
            bool seenShade = false;
            for (int s = 0; s < sSeenShadeCount; s++) {
                if (sSeenShadeStates[s] == shadeKey) {
                    seenShade = true;
                    break;
                }
            }
            if (!seenShade && sSeenShadeCount < 64) {
                sSeenShadeStates[sSeenShadeCount++] = shadeKey;
                /* Sum the first 64 source bytes at draw time: zero convicts slot
                   replacement between load and draw, nonzero points at UV/handle. */
                const int probeTile = mRdp->first_tile_index;
                const uint32_t probeTmem = mRdp->texture_tile[probeTile].tmem_index;
                const uint8_t* probeSrc = mRdp->LoadedTextureAt(probeTmem).addr;
                const uint32_t probeSize = mRdp->LoadedTextureAt(probeTmem).size_bytes;
                uint32_t srcSum = 0;
                if (probeSrc != nullptr) {
                    const uint32_t n = probeSize < 64 ? probeSize : 64;
                    for (uint32_t k = 0; k < n; k++) {
                        srcSum = srcSum * 31 + probeSrc[k];
                    }
                }
                gdx_dbg_logf("[fontmach] shade-src tile=%d tmem=0x%X addr=%p sizeB=%u sum=%08X\n",
                             probeTile, probeTmem, static_cast<const void*>(probeSrc), probeSize, srcSum);
                gdx_dbg_logf("[fontmach] shade lit=%d n=%d v0=(%u,%u,%u,%u) amb=(%u,%u,%u) "
                             "l0=(%u,%u,%u dir %d,%d,%d) env=(%u,%u,%u,%u) cc=%016llX "
                             "tile=%u st0=(%.1f,%.1f) st1=(%.1f,%.1f) st2=(%.1f,%.1f)\n",
                             lit ? 1 : 0, numLights, v1->color.r, v1->color.g, v1->color.b, v1->color.a,
                             amb.col[0], amb.col[1], amb.col[2], l0.col[0], l0.col[1], l0.col[2],
                             (int)l0.dir[0], (int)l0.dir[1], (int)l0.dir[2], mRdp->env_color.r,
                             mRdp->env_color.g, mRdp->env_color.b, mRdp->env_color.a,
                             (unsigned long long)mRdp->combine_mode, (unsigned)mRdp->first_tile_index,
                             v1->u, v1->v, v2->u, v2->v, v3->u, v3->v);
            }
        }
    }

    // The F3DEX2.Rej family does not clip: it drops any triangle with a vertex outside the reject
    // box (twice the viewport extent in X/Y) or behind the eye. F-Zero X relies on this for the
    // machine screens and FLX vehicles; rasterizing those triangles paints giant wedges.
    if (!is_rect && sClipFanDepth == 0 &&
        (mF3dex2Variant == F3dex2Variant::Reject || mF3dex2Variant == F3dex2Variant::FZeroFlxReject)) {
        for (const LoadedVertex* vertex : v_arr) {
            const float w2 = 2.0f * vertex->w;
            if (vertex->w <= 0.0f || vertex->x < -w2 || vertex->x > w2 || vertex->y < -w2 ||
                vertex->y > w2 || vertex->z < -vertex->w) {
                mGeometryDiagnostics.trianglesClipRejected++;
                GDX_TRI2_CNT(Early);
                GDX_TRI2_LAP(Pre);
                return;
            }
        }
    }

    // Clip in homogeneous space before culling or perspective division. F3DEX/F3DFLX display
    // lists routinely straddle the eye and frustum planes, and a modern backend renders those as
    // giant wedges instead of the RSP-clipped polygons.
    constexpr float kMinClipW = kGdxMinClipW;
    const auto clipDistance = [=](const LoadedVertex& vertex, uint32_t plane) {
        switch (plane) {
            case 0:
                return vertex.w - kMinClipW;
            case 1:
                return vertex.x + vertex.w;
            case 2:
                return vertex.w - vertex.x;
            case 3:
                return vertex.y + vertex.w;
            case 4:
                return vertex.w - vertex.y;
            case 5:
                return vertex.z + vertex.w;
            default:
                return vertex.w - vertex.z;
        }
    };
    // [traffic] The old pre-scan evaluated clipDistance() for up to 7 planes x 3 vertices
    // through the switch-lambda on EVERY triangle, fully-inside ones included (~860 race tris
    // per traffic frame paid the whole 21-test walk). The per-vertex clip_out mask holds the
    // same 7 outside-tests, computed once at vertex-transform time with identical arithmetic,
    // so the pre-scan collapses to a 3-byte OR. requiresClipping is bit-for-bit the old value:
    // clip_out bit p is set iff clipDistance(v, p) < 0.
    bool requiresClipping = false;
    if (!is_rect && sClipFanDepth == 0) {
        requiresClipping = ((v1->clip_out | v2->clip_out | v3->clip_out) != 0);
    }

    if (requiresClipping) {
        LoadedVertex polygonA[16] = { *v1, *v2, *v3 };
        LoadedVertex polygonB[16]{};
        LoadedVertex* input = polygonA;
        LoadedVertex* output = polygonB;
        size_t inputCount = 3;

        for (uint32_t plane = 0; plane < 7 && inputCount >= 3; ++plane) {
            size_t outputCount = 0;
            for (size_t i = 0; i < inputCount; ++i) {
                const LoadedVertex& current = input[i];
                const LoadedVertex& next = input[(i + 1) % inputCount];
                const float currentDistance = clipDistance(current, plane);
                const float nextDistance = clipDistance(next, plane);
                const bool currentInside = currentDistance >= 0.0f;
                const bool nextInside = nextDistance >= 0.0f;

                if (currentInside && outputCount < 16) {
                    output[outputCount++] = current;
                }
                if (currentInside != nextInside && outputCount < 16) {
                    const float denominator = currentDistance - nextDistance;
                    const float t = denominator != 0.0f ? currentDistance / denominator : 0.0f;
                    output[outputCount++] = InterpolateLoadedVertex(current, next, std::clamp(t, 0.0f, 1.0f));
                }
            }
            inputCount = outputCount;
            std::swap(input, output);
        }

        if (inputCount < 3) {
            mGeometryDiagnostics.trianglesClipRejected++;
            GDX_TRI2_CNT(Early);
            GDX_TRI2_LAP(Pre);
            return;
        }

        LoadedVertex saved[3];
        for (size_t i = 0; i < 3; ++i) {
            saved[i] = mRsp->loaded_vertices[MAX_VERTICES + i];
        }

        // Three reserved vertex slots suffice regardless of polygon size.
        mGeometryDiagnostics.trianglesSubmitted--;
        ++sClipFanDepth;
        for (size_t i = 1; i + 1 < inputCount; ++i) {
            mRsp->loaded_vertices[MAX_VERTICES] = input[0];
            mRsp->loaded_vertices[MAX_VERTICES + 1] = input[i];
            mRsp->loaded_vertices[MAX_VERTICES + 2] = input[i + 1];
            for (size_t j = 0; j < 3; ++j) {
                mRsp->loaded_vertices[MAX_VERTICES + j].w =
                    std::max(mRsp->loaded_vertices[MAX_VERTICES + j].w, kMinClipW);
                UpdateLoadedVertexClipFlags(mRsp->loaded_vertices[MAX_VERTICES + j]);
            }
            GfxSpTri1(MAX_VERTICES, MAX_VERTICES + 1, MAX_VERTICES + 2, false);
        }
        --sClipFanDepth;

        for (size_t i = 0; i < 3; ++i) {
            mRsp->loaded_vertices[MAX_VERTICES + i] = saved[i];
        }
        GDX_TRI2_CNT(Fan);
        GDX_TRI2_LAP(Pre);
        return;
    }

    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        // The whole triangle lies outside the visible area
        if (!is_rect) {
            mGeometryDiagnostics.trianglesClipRejected++;
        }
        GDX_TRI2_CNT(Early);
        GDX_TRI2_LAP(Pre);
        return;
    }

    const uint32_t cull_both = get_attr(CULL_BOTH);
    const uint32_t cull_front = get_attr(CULL_FRONT);
    const uint32_t cull_back = get_attr(CULL_BACK);

    if ((mRsp->geometry_mode & cull_both) != 0) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;

        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }

        // G_EX_INVERT_CULLING is a LUS extension, not tied to a specific ucode,
        // so apply it regardless of the active microcode handler.
        if ((mRsp->extra_geometry_mode & G_EX_INVERT_CULLING) != 0) {
            cross = -cross;
        }

        auto cull_type = mRsp->geometry_mode & cull_both;

        if (cull_type == cull_front) {
            if (cross <= 0) {
                if (!is_rect) {
                    mGeometryDiagnostics.trianglesCullRejected++;
                }
                GDX_TRI2_CNT(Early);
                GDX_TRI2_LAP(Pre);
                return;
            }
        } else if (cull_type == cull_back) {
            if (cross >= 0) {
                if (!is_rect) {
                    mGeometryDiagnostics.trianglesCullRejected++;
                }
                GDX_TRI2_CNT(Early);
                GDX_TRI2_LAP(Pre);
                return;
            }
        } else if (cull_type == cull_both) {
            // Why is this even an option?
            if (!is_rect) {
                mGeometryDiagnostics.trianglesCullRejected++;
            }
            GDX_TRI2_CNT(Early);
            GDX_TRI2_LAP(Pre);
            return;
        }
    }
    GDX_TRI2_LAP(Pre);

    // Capture the first oversized triangle that survives Reject-variant screening each frame.
    // Legit machine-select geometry stays under ~0.2 NDC extent, so anything near the reject-box
    // limit is a defect candidate worth identifying by material.
    if (!is_rect && gdx_s7_geo_diag_enabled &&
        (mF3dex2Variant == F3dex2Variant::Reject || mF3dex2Variant == F3dex2Variant::FZeroFlxReject)) {
        float extent = 0.0f;
        for (const LoadedVertex* vertex : v_arr) {
            if (vertex->w > 1.0e-6f) {
                extent = std::max(extent, std::max(fabsf(vertex->x), fabsf(vertex->y)) / vertex->w);
            }
        }
        if (extent > 0.9f) {
            if (mGeometryDiagnostics.bigTriangles++ == 0) {
                for (int i = 0; i < 3; i++) {
                    mGeometryDiagnostics.bigTriX[i] = v_arr[i]->x;
                    mGeometryDiagnostics.bigTriY[i] = v_arr[i]->y;
                    mGeometryDiagnostics.bigTriZ[i] = v_arr[i]->z;
                    mGeometryDiagnostics.bigTriW[i] = v_arr[i]->w;
                }
                mGeometryDiagnostics.bigTriGeometryMode = mRsp->geometry_mode;
                mGeometryDiagnostics.bigTriCombine = mRdp->combine_mode;
                const uint32_t bigTriTile = mRdp->first_tile_index;
                mGeometryDiagnostics.bigTriTile = static_cast<uint8_t>(bigTriTile);
                mGeometryDiagnostics.bigTriTexture =
                    mRdp->LoadedTextureAt(mRdp->texture_tile[bigTriTile].tmem_index).addr;
                mGeometryDiagnostics.bigTriViewportX = mRdp->viewport.x;
                mGeometryDiagnostics.bigTriViewportY = mRdp->viewport.y;
                mGeometryDiagnostics.bigTriViewportW = mRdp->viewport.width;
                mGeometryDiagnostics.bigTriViewportH = mRdp->viewport.height;
            }
        }
    }

    // depth_test is set when the fragment has a depth value to compare (either from vertex Z via
    // RSP G_ZBUFFER, or from the prim-depth register via G_ZS_PRIM) and Z_CMP is requested.
    bool zbuffer_enabled = (mRsp->geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    bool prim_depth_enabled = (mRdp->other_mode_l & G_ZS_PRIM) != 0;
    bool depth_test = (zbuffer_enabled || prim_depth_enabled) && (mRdp->other_mode_l & Z_CMP) == Z_CMP;
    bool depth_mask = (mRdp->other_mode_l & Z_UPD) == Z_UPD;
    static const bool diagnosticDisablePreFlxDepth = std::getenv("GDX_DIAG_DISABLE_PREFLX_DEPTH") != nullptr;
    if (diagnosticDisablePreFlxDepth && mF3dex2Variant != F3dex2Variant::FZeroFlxReject && depth_test) {
        depth_test = false;
        depth_mask = false;
        mGeometryDiagnostics.depthBypassTriangles++;
    }
    uint8_t depth_test_and_mask = (depth_test ? 1 : 0) | (depth_mask ? 2 : 0);
    if (depth_test_and_mask != mRenderingState.depth_test_and_mask) {
        Flush();
        mRapi->SetDepthTestAndMask(depth_test, depth_mask);
        mRenderingState.depth_test_and_mask = depth_test_and_mask;
    }

    bool zmode_decal = (mRdp->other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != mRenderingState.decal_mode) {
        Flush();
        mRapi->SetZmodeDecal(zmode_decal);
        mRenderingState.decal_mode = zmode_decal;
    }

    if (mRdp->viewport_or_scissor_changed) {
        if (memcmp(&mRdp->viewport, &mRenderingState.viewport, sizeof(mRdp->viewport)) != 0) {
            Flush();
            mRapi->SetViewport(mRdp->viewport.x, mRdp->viewport.y, mRdp->viewport.width, mRdp->viewport.height);
            mRenderingState.viewport = mRdp->viewport;
        }
        if (memcmp(&mRdp->scissor, &mRenderingState.scissor, sizeof(mRdp->scissor)) != 0) {
            Flush();
            mRapi->SetScissor(mRdp->scissor.x, mRdp->scissor.y, mRdp->scissor.width, mRdp->scissor.height);
            mRenderingState.scissor = mRdp->scissor;
        }
        mRdp->viewport_or_scissor_changed = false;
    }

    uint64_t cc_id = mRdp->combine_mode;
    uint64_t cc_options = 0;
    bool use_alpha = ((mRdp->other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20) &&
                      (mRdp->other_mode_l & (3 << 16)) == (G_BL_1MA << 16)) ||
                     ((mRdp->other_mode_l & (3 << 22)) == (G_BL_CLR_MEM << 22) &&
                      (mRdp->other_mode_l & (3 << 18)) == (G_BL_1MA << 18));
    uint8_t blend_src = mRdp->other_mode_l >> 30;
    bool use_blend_color = blend_src == G_BL_CLR_BL;
    bool use_fog = blend_src == G_BL_CLR_FOG || use_blend_color;
    const bool originalUseFog = use_fog;
    bool texture_edge = (mRdp->other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (mRdp->other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_DITHER;
    bool use_2cyc = (mRdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    bool alpha_threshold = (mRdp->other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    bool invisible =
        (mRdp->other_mode_l & (3 << 24)) == (G_BL_0 << 24) && (mRdp->other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20);
    if (invisible && !is_rect) {
        mGeometryDiagnostics.trianglesInvisible++;
    }
    bool use_grayscale = mRdp->grayscale;
    bool use_prim_depth = (mRdp->other_mode_l & G_ZS_PRIM) != 0;
    static const bool diagnosticDisablePreFlxFog =
        std::getenv("GDX_DIAG_DISABLE_PREFLX_FOG") != nullptr;
    const bool bypassFog =
        diagnosticDisablePreFlxFog && mF3dex2Variant != F3dex2Variant::FZeroFlxReject && originalUseFog && !is_rect;
    if (bypassFog) {
        use_fog = false;
    }
    // Create Machine flat-navy preview: skipping the gDPSetRenderMode call at
    // machine_create_draw.c:655 fixes it, but the mechanism is unproven. Log one line per distinct
    // mode tuple to diff against the machine-settings screen, which runs the same display lists
    // correctly. getenv rather than gdx_dev_gates, which is not visible from this TU.
    static const bool diagnosticBlendMode = std::getenv("GDX_DIAG_BLENDMODE") != nullptr;
    if (diagnosticBlendMode) {
        static std::set<std::pair<uint64_t, uint64_t>> sSeenModes;
        static int sModeLogs = 0;
        const auto key = std::make_pair(static_cast<uint64_t>(mRdp->other_mode_l), cc_id);

        if (sModeLogs < 48 && sSeenModes.insert(key).second) {
            ++sModeLogs;
            // cc_id packs colour in the low 16 bits and alpha above it.
            gdx_dbg_logf("[blendmode] other_l=%08X cc=%016llX ccAlpha=%04X use_alpha=%d decal=%d "
                         "forceBl=%d zmode=%X\n",
                         (unsigned) mRdp->other_mode_l, (unsigned long long) cc_id,
                         (unsigned) ((cc_id >> 16) & 0xFFFF), use_alpha ? 1 : 0,
                         ((mRdp->other_mode_l & ZMODE_DEC) == ZMODE_DEC) ? 1 : 0,
                         (mRdp->other_mode_l & FORCE_BL) ? 1 : 0,
                         (unsigned) ((mRdp->other_mode_l >> 10) & 3));
        }
    }

    static const bool diagnosticForcePreFlxSimpleMaterial =
        std::getenv("GDX_DIAG_FORCE_PREFLX_SIMPLE_MATERIAL") != nullptr;
    const bool forceSimpleMaterial =
        diagnosticForcePreFlxSimpleMaterial && mF3dex2Variant != F3dex2Variant::FZeroFlxReject && !is_rect;

    if (forceSimpleMaterial) {
        // A/B diagnostic: bypass original combiner/blender interpretation while
        // preserving geometry, texture state, UVs, and vertex shade color.
        cc_id = color_comb(G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_SHADE, G_CCMUX_0) |
                (static_cast<uint64_t>(
                     alpha_comb(G_ACMUX_TEXEL0, G_ACMUX_0, G_ACMUX_SHADE, G_ACMUX_0))
                 << 16);
        use_alpha = false;
        use_fog = false;
        texture_edge = false;
        use_noise = false;
        use_2cyc = false;
        alpha_threshold = false;
        invisible = false;
        use_grayscale = false;
        use_prim_depth = false;
    }

    if (texture_edge) {
        if (use_alpha) {
            alpha_threshold = true;
            texture_edge = false;
        }
        use_alpha = true;
    }

    // Alpha compare (G_AC_THRESHOLD / G_AC_DITHER) rejects fragments by texel alpha even when the
    // blender itself is opaque -- COPY-mode HUD sprite blits rely on it. The shader only compiles
    // the discard under the ALPHA option, so alpha compare must force it on, like texture_edge.
    if (alpha_threshold || use_noise) {
        use_alpha = true;
    }

    if (use_alpha) {
        cc_options |= SHADER_OPT(ALPHA);
    }
    if (use_fog) {
        cc_options |= SHADER_OPT(FOG);
    }
    if (texture_edge) {
        cc_options |= SHADER_OPT(TEXTURE_EDGE);
    }
    if (use_noise) {
        cc_options |= SHADER_OPT(NOISE);
    }
    if (use_2cyc) {
        cc_options |= SHADER_OPT(_2CYC);
    }
    if (alpha_threshold) {
        cc_options |= SHADER_OPT(ALPHA_THRESHOLD);
    }
    if (invisible) {
        cc_options |= SHADER_OPT(INVISIBLE);
    }
    if (use_grayscale) {
        cc_options |= SHADER_OPT(GRAYSCALE);
    }
    if (use_prim_depth) {
        cc_options |= SHADER_OPT(PRIM_DEPTH);
    }

    if (!mShaderStack.empty()) {
        cc_options |= (mShaderStack.top() << SHADER_ID_SHIFT);
    } else {
        cc_options |= -1 << SHADER_ID_SHIFT;
    }

    const uint32_t texel0Tile = mRdp->first_tile_index;
    // In two-cycle mode TEXEL1 uses the next tile descriptor. F-Zero X relies on this for track
    // and vehicle materials, where adjacent tiles describe different views of one TMEM load.
    // GDX_DIAG_TEXEL1_FROM_BASE forces TEXEL1 back to the base tile, to isolate whether
    // track-floor corruption comes from that adjacent-tile view.
    static const bool diagnosticTexel1FromBase = std::getenv("GDX_DIAG_TEXEL1_FROM_BASE") != nullptr;
    const uint32_t texel1Tile =
        diagnosticTexel1FromBase ? texel0Tile : ((mRdp->first_tile_index + 1u) & 7u);
    const auto& texel0Loaded = mRdp->LoadedTextureAt(mRdp->texture_tile[texel0Tile].tmem_index);
    const auto& texel1Loaded = mRdp->LoadedTextureAt(mRdp->texture_tile[texel1Tile].tmem_index);

    if (texel0Loaded.masked) {
        cc_options |= SHADER_OPT(TEXEL0_MASK);
    }
    if (texel1Loaded.masked) {
        cc_options |= SHADER_OPT(TEXEL1_MASK);
    }
    if (texel0Loaded.blended) {
        cc_options |= SHADER_OPT(TEXEL0_BLEND);
    }
    if (texel1Loaded.blended) {
        cc_options |= SHADER_OPT(TEXEL1_BLEND);
    }

    // LEAK ROOT CAUSE (3DS first hardware OOM, heapUsed 38.7->75.5MB over a session):
    // ColorCombinerKey grew a third field (shader_id, upstream "fix windows arm CI
    // failure") that NO construction site ever writes and nothing ever reads — so this
    // stack local carried garbage into the map key. operator<=> compares all three
    // fields: whenever the garbage differed from the pooled entries', the lookup MISSED
    // and inserted a brand-new 148-byte Rb-tree node into mColorCombinerPool (which has
    // no eviction — by design, unique combiners are finite), plus paid a batch-breaking
    // Flush() and a full GenerateCC. Measured on the 3DS: ~14 leaked nodes per race
    // frame, +11 MB live in ~5300 frames, the whole census-invisible heap climb.
    // Value-initialize so shader_id compares equal (0) everywhere, restoring the
    // intended ~few-dozen-entry pool.
    ColorCombinerKey key{};
    key.combine_mode = cc_id;
    key.options = cc_options;

    ColorCombiner* comb = LookupOrCreateColorCombiner(key);
    GDX_TRI2_LAP(State);

    // GDX_DIAG_EFFECTDRAW: the PIT/HEAL strip draws flat tan despite a proven TMEM identity. Log
    // the combine id and the texel slots the combiner uses, to confirm or rule out two-cycle
    // TEXEL1 sampling tile+1, which for PIT is DIRT's I4/IA4 mask.
    static const bool sDiagEffectDraw = std::getenv("GDX_DIAG_EFFECTDRAW") != nullptr;
    if (sDiagEffectDraw && gGdxRaceActive != 0 && texel0Tile >= 1 && texel0Tile <= 4) {
        static int sEffectDrawStateLogs = 0;
        if (sEffectDrawStateLogs < 64) {
            ++sEffectDrawStateLogs;
            SPDLOG_ERROR("[effect-draw-state] tile0={} tile1={} cc_id={:#x} usedTex0={} usedTex1={} "
                         "tile0_fmt={} tile0_siz={} tile0_cms={} tile0_cmt={} tile1_fmt={} tile1_siz={} "
                         "tile1_cms={} tile1_cmt={} 2cyc={}",
                         texel0Tile, texel1Tile, cc_id, comb->usedTextures[0], comb->usedTextures[1],
                         mRdp->texture_tile[texel0Tile].fmt, mRdp->texture_tile[texel0Tile].siz,
                         mRdp->texture_tile[texel0Tile].cms, mRdp->texture_tile[texel0Tile].cmt,
                         mRdp->texture_tile[texel1Tile].fmt, mRdp->texture_tile[texel1Tile].siz,
                         mRdp->texture_tile[texel1Tile].cms, mRdp->texture_tile[texel1Tile].cmt, use_2cyc);
        }
    }

    // GDX_MINIMAP_PROBE=1: confirm the texture filter actually in effect and the
    // resolved combiner for the CI8 minimap rects. Pairs with [minimap-ci8] above.
    static const bool sMinimapDrawProbe = std::getenv("GDX_MINIMAP_PROBE") != nullptr;
    if (sMinimapDrawProbe && is_rect &&
        mRdp->texture_tile[texel0Tile].fmt == G_IM_FMT_CI &&
        mRdp->texture_tile[texel0Tile].siz == G_IM_SIZ_8b) {
        static int sMinimapDrawLogs = 0;
        if (sMinimapDrawLogs < 32) {
            ++sMinimapDrawLogs;
            uint32_t textfilt = (mRdp->other_mode_h >> G_MDSFT_TEXTFILT) & 3u;
            uint32_t tile_w = (uint32_t)((mRdp->texture_tile[texel0Tile].lrs -
                                          mRdp->texture_tile[texel0Tile].uls + 4) / 4);
            uint32_t tile_h = (uint32_t)((mRdp->texture_tile[texel0Tile].lrt -
                                          mRdp->texture_tile[texel0Tile].ult + 4) / 4);
            SPDLOG_ERROR("[minimap-draw] is_rect cc_id={:#x} usedTex0={} usedTex1={} "
                         "textfilt={} (0=POINT 2=BILERP 3=AVERAGE) fmt={} siz={} cms={} cmt={} tile={}x{}",
                         cc_id, comb->usedTextures[0], comb->usedTextures[1], textfilt,
                         mRdp->texture_tile[texel0Tile].fmt, mRdp->texture_tile[texel0Tile].siz,
                         mRdp->texture_tile[texel0Tile].cms, mRdp->texture_tile[texel0Tile].cmt,
                         tile_w, tile_h);
        }
    }

    uint32_t tm = 0;
    uint32_t tex_width[2] = {}, tex_height[2] = {}, tex_width2[2] = {}, tex_height2[2] = {};
    uint32_t effective_tile[2] = {};

    // [perf-s7] Reuse the tile-extent derivation while the draw state is clean (see
    // GfxS7OpcodePreservesDrawState / sGfxS7DirtyMask). The comb identity check pins the
    // memo to the combiner it was derived under (usedTextures gates the per-unit work), and the
    // live textures_changed probe closes the cache-eviction hole: an eviction inside an import
    // can re-arm textures_changed without an intervening state opcode.
    // [texrect] Rect draws consume/bank their own memo instance under their own (wider-
    // whitelist, see GfxS7OpcodePreservesRectState) dirty bit, additionally keyed on
    // first_tile_index: the texrect handlers swap the tile per rect, and a tile switch changes
    // extents/samplers without any dirtying opcode in between. A switch away from the game's
    // tile also re-arms textures_changed on rect exit, which the live probe catches; the
    // first_tile key covers the switch BACK, where textures_changed stays clear.
    GfxS7TriStateMemo& s7Memo = is_rect ? sGfxS7RectMemo : sGfxS7TriMemo;
    const uint8_t s7DirtyBit = is_rect ? kGfxS7DirtyRect : kGfxS7DirtyTri;
    // [trifast] per-unit textures_changed test (see the block comment at gdx_trifast_stat).
    const bool s7TcLegacyOk = !mRdp->textures_changed[0] && !mRdp->textures_changed[1];
    const bool s7TcOk = sGdxTriFast ? ((!comb->usedTextures[0] || !mRdp->textures_changed[0]) &&
                                       (!comb->usedTextures[1] || !mRdp->textures_changed[1]))
                                    : s7TcLegacyOk;
    const bool s7TexMemoHit = (sGfxS7DirtyMask & s7DirtyBit) == 0 && s7Memo.valid &&
                              s7Memo.comb == static_cast<const void*>(comb) &&
                              (!is_rect || s7Memo.first_tile == mRdp->first_tile_index) && s7TcOk;
    // [trifast] verify: a hit the legacy predicate refuses is re-derived by the slow path below
    // and compared against the banked record afterwards (GdxS7VerifyBank).
    const bool s7VerifyNewHit = sGdxTriFastVerify && s7TexMemoHit && !s7TcLegacyOk;
    GfxS7TriStateMemo s7VerifyBank;
    if (s7VerifyNewHit) {
        s7VerifyBank = s7Memo;
    }
    if (s7TexMemoHit && !s7VerifyNewHit) {
        GDX_TRI2_CNT(MemoHit);
        gdx_trifast_stat[kGdxTfHit]++;
        if (!s7TcLegacyOk) {
            gdx_trifast_stat[kGdxTfHitNew]++;
        }
        tm = s7Memo.tm;
        for (int i = 0; i < 2; i++) {
            tex_width[i] = s7Memo.tex_width[i];
            tex_height[i] = s7Memo.tex_height[i];
            tex_width2[i] = s7Memo.tex_width2[i];
            tex_height2[i] = s7Memo.tex_height2[i];
            effective_tile[i] = s7Memo.effective_tile[i];
        }
    } else {
    GDX_TRI2_CNT(MemoMiss);
    // [trifast] receipt: first failing test, legacy order (verify re-derivations are hits).
    if (!s7VerifyNewHit)
    gdx_trifast_stat[(sGfxS7DirtyMask & s7DirtyBit) != 0 ? kGdxTfMissDirty
                     : !s7Memo.valid                      ? kGdxTfMissInvalid
                     : s7Memo.comb != static_cast<const void*>(comb) ? kGdxTfMissComb
                     : (is_rect && s7Memo.first_tile != mRdp->first_tile_index) ? kGdxTfMissTile
                     : mRdp->textures_changed[0] ? kGdxTfMissTc0
                                                 : kGdxTfMissTc1]++;
    for (int i = 0; i < 2; i++) {
        uint32_t tile = (i == 1) ? texel1Tile : texel0Tile;
        effective_tile[i] = tile;

        if (comb->usedTextures[i]) {
            const auto& activeLoaded = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index);
            if (mRdp->textures_changed[i]) {
#ifdef __3DS__
                // [trectbatch] Rect draws: import FIRST (arming the atlas for an eligible rect
                // texture), then keep the open packed batch when the resolved texture is a view
                // on the page already bound (same GPU texture; the pack params are refreshed
                // for the new view's UV offset and the new view inherits the batch's sampler
                // state so a later sampler split still draws the pending tris right). Any
                // other outcome draws the pending batch with the texture it was built for
                // (rebind the previous id around the Flush — the 3DS backend's SelectTexture
                // only records the id; C3D_TexBind happens in DrawTriangles). Guards keep every
                // other case on the legacy order: killswitch off, non-rect, empty or
                // non-packed batch, unit unbound, masked/blended units, cache at capacity (an
                // eviction could free the pending batch's id for this very import to reuse).
                TextureCacheNode* const trectPrevNode = mRenderingState.mTextures[i];
                const bool trectRect = sTrectBatchOn && is_rect;
                const bool trectDefer = trectRect && mBufVboLen > 0 && sGdxPackBatch && !sGdxPackFolded &&
                                        trectPrevNode != nullptr &&
                                        !activeLoaded.masked && !activeLoaded.blended &&
                                        mTextureCache.map.size() + 1 < TEXTURE_CACHE_MAX_SIZE;
                if (trectRect && mBufVboLen > 0 && !trectDefer) {
                    sTrectBatchBypassed++;
                }
                const uint32_t trectPrevId = trectDefer ? trectPrevNode->second.texture_id : 0u;
                const int trectPrevPage = trectDefer ? gdx3ds_tex_page(trectPrevId) : -1;
                if (!trectDefer) {
                    Flush();
                }
                // Unit 0 only: eligibility was judged from the rect's own tile, not texel1Tile.
                const bool trectArm = trectRect && i == 0 && sTrectRectAtlasOk && !activeLoaded.masked && !activeLoaded.blended;
                if (trectArm) {
                    gdx3ds_atlas_arm(1);
                    sTrectBatchArmed++;
                }
                ImportTexture(i, tile, false);
                if (trectArm) {
                    gdx3ds_atlas_arm(0);
                }
                if (trectDefer) {
                    TextureCacheNode* const trectNewNode = mRenderingState.mTextures[i];
                    const uint32_t trectNewId = trectNewNode != nullptr ? trectNewNode->second.texture_id : 0u;
                    const int trectNewPage = trectNewNode != nullptr ? gdx3ds_tex_page(trectNewId) : -1;
                    if (trectNewNode == trectPrevNode || (trectPrevPage >= 0 && trectNewPage == trectPrevPage)) {
                        sTrectBatchMerged++;
                        if (trectNewNode != trectPrevNode) {
                            mRapi->SetSamplerParameters(i, mRenderingState.sampler_linear_filter[i],
                                                        mRenderingState.sampler_cms[i], mRenderingState.sampler_cmt[i]);
                            gdx3ds_vbopack_refresh(&sGdxPackParams);
                        }
                    } else {
                        mRapi->SelectTexture(i, trectPrevId);
                        Flush();
                        if (trectNewNode != nullptr) { // a failed import leaves the previous binding, as legacy does
                            mRapi->SelectTexture(i, trectNewId);
                        }
                        sTrectBatchSwitched++;
                    }
                } else if (!is_rect && sTrectBatchOn && GdxTrectActive() && mRenderingState.mTextures[i] != nullptr &&
                           gdx3ds_tex_page(mRenderingState.mTextures[i]->second.texture_id) >= 0) {
                    sTrectViewTriDraws++; // diagnostic only: a game triangle sampling an atlas view
                }
#else
                Flush();
                ImportTexture(i, tile, false);
#endif
                if (activeLoaded.masked) {
                    ImportTextureMask(SHADER_FIRST_MASK_TEXTURE + i, tile);
                }
                if (activeLoaded.blended) {
                    ImportTexture(SHADER_FIRST_REPLACEMENT_TEXTURE + i, tile, true);
                }
                mRdp->textures_changed[i] = false;
            }

            uint8_t cms = mRdp->texture_tile[tile].cms;
            uint8_t cmt = mRdp->texture_tile[tile].cmt;

            uint32_t loaded_line_size = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).line_size_bytes;
            uint32_t loaded_size = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).size_bytes;
            uint32_t loaded_full_line =
                mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).full_image_line_size_bytes;
            uint32_t tex_size_bytes;
            uint32_t line_size;
            if ((loaded_line_size != loaded_size || loaded_full_line != loaded_size) && loaded_line_size > 0) {
                line_size = loaded_line_size;
                tex_size_bytes = loaded_size;
            } else {
                line_size = mRdp->texture_tile[tile].line_size_bytes;
                tex_size_bytes = mRdp->LoadedTextureAt(mRdp->texture_tile[tile].tmem_index).orig_size_bytes;
                // RGBA32: texture_tile stores TMEM-interleaved stride (half of actual DRAM stride).
                if (mRdp->texture_tile[tile].siz == G_IM_SIZ_32b) {
                    line_size *= 2;
                }
            }

            if (line_size == 0) {
                line_size = 1;
            }

            tex_height[i] = tex_size_bytes / line_size;
            switch (mRdp->texture_tile[tile].siz) {
                case G_IM_SIZ_4b:
                    line_size <<= 1;
                    break;
                case G_IM_SIZ_8b:
                    break;
                case G_IM_SIZ_16b:
                    line_size /= G_IM_SIZ_16b_LINE_BYTES;
                    break;
                case G_IM_SIZ_32b:
                    line_size /= 4; // RGBA32: 4 bytes per pixel (line_size is now actual DRAM stride)
                    break;
            }
            tex_width[i] = line_size;

            tex_width2[i] = (uint32_t)(int32_t)((mRdp->texture_tile[tile].lrs - mRdp->texture_tile[tile].uls + 4) / 4);
            tex_height2[i] = (uint32_t)(int32_t)((mRdp->texture_tile[tile].lrt - mRdp->texture_tile[tile].ult + 4) / 4);

            // Same pyramid-like ratio gate as ImportTexture: only clamp when loaded pixels
            // are close to rendered pixels (mipmap), not when much bigger (window scroll).
            uint32_t loadedPx = tex_width[i] * tex_height[i];
            uint32_t renderedPx = tex_width2[i] * tex_height2[i];
            bool pyrLike = renderedPx > 0 && loadedPx > renderedPx && loadedPx * 8 < renderedPx * 13;
            if ((pyrLike || (cms & G_TX_CLAMP)) && tex_width2[i] > 0 && tex_width2[i] < tex_width[i]) {
                tex_width[i] = tex_width2[i];
            }
            if ((pyrLike || (cmt & G_TX_CLAMP)) && tex_height2[i] > 0 && tex_height2[i] < tex_height[i]) {
                tex_height[i] = tex_height2[i];
            }
            ApplyTileMaskExtent(mRdp, tile, tex_width[i], tex_height[i], /*maskAuthoritative=*/true);
            /* One line per distinct draw-time UV-extent state: tex_w/h feed texcoord
               normalization, and a mismatch against the uploaded decode extent is the bug. */
            {
                static const bool sDiagFontMachineD = std::getenv("GDX_DIAG_FONT_MACHINE") != nullptr;
                if (sDiagFontMachineD) {
                    const uint64_t drawKey =
                        (static_cast<uint64_t>(tile) << 56) |
                        (static_cast<uint64_t>(mRdp->texture_tile[tile].tmem_index) << 40) |
                        (static_cast<uint64_t>(mRdp->texture_tile[tile].fmt) << 37) |
                        (static_cast<uint64_t>(mRdp->texture_tile[tile].siz) << 35) |
                        (static_cast<uint64_t>(tex_width[i] & 0x7FF) << 24) |
                        (static_cast<uint64_t>(tex_height[i] & 0x7FF) << 13) |
                        (static_cast<uint64_t>(tex_width2[i] & 0x3F) << 7) | (tex_height2[i] & 0x7F);
                    static uint64_t sSeenDraws[128] = {};
                    static int sSeenDrawCount = 0;
                    bool seenDraw = false;
                    for (int s = 0; s < sSeenDrawCount; s++) {
                        if (sSeenDraws[s] == drawKey) {
                            seenDraw = true;
                            break;
                        }
                    }
                    if (!seenDraw && sSeenDrawCount < 128) {
                        sSeenDraws[sSeenDrawCount++] = drawKey;
                        gdx_dbg_logf("[fontmach] draw tile=%u tmem=0x%X fmt=%u siz=%u texWH=%ux%u "
                                     "tileWH=%ux%u mask=%u/%u shift=%u/%u cm=%u/%u rect=%d scaleST=%X/%X\n",
                                     tile, mRdp->texture_tile[tile].tmem_index, mRdp->texture_tile[tile].fmt,
                                     mRdp->texture_tile[tile].siz, tex_width[i], tex_height[i], tex_width2[i],
                                     tex_height2[i], mRdp->texture_tile[tile].masks, mRdp->texture_tile[tile].maskt,
                                     mRdp->texture_tile[tile].shifts, mRdp->texture_tile[tile].shiftt,
                                     mRdp->texture_tile[tile].cms, mRdp->texture_tile[tile].cmt, is_rect ? 1 : 0,
                                     mRsp->texture_scaling_factor.s, mRsp->texture_scaling_factor.t);
                    }
                }
            }
            // Degenerate load bookkeeping (a slot whose recorded byte count is smaller than one
            // line) yields a zero extent, and the texcoord normalization below would divide by
            // zero and emit infinite UVs. The tile mask is the hardware wrap period, so prefer it;
            // otherwise clamp to a single texel.
            if (tex_width[i] == 0) {
                const uint8_t masksFix = mRdp->texture_tile[tile].masks;
                tex_width[i] = (masksFix != G_TX_NOMASK && masksFix < 31) ? (1u << masksFix) : 1u;
            }
            if (tex_height[i] == 0) {
                const uint8_t masktFix = mRdp->texture_tile[tile].maskt;
                tex_height[i] = (masktFix != G_TX_NOMASK && masktFix < 31) ? (1u << masktFix) : 1u;
            }
            if (!is_rect && i == 0) {
                mGeometryDiagnostics.textureWidth = tex_width[i];
                mGeometryDiagnostics.textureHeight = tex_height[i];
                mGeometryDiagnostics.textureLineBytes = loaded_line_size;
                mGeometryDiagnostics.textureSizeBytes = loaded_size;
                mGeometryDiagnostics.textureTmem = mRdp->texture_tile[tile].tmem_index;
                mGeometryDiagnostics.textureTile = static_cast<uint8_t>(tile);
                mGeometryDiagnostics.textureMaskS = mRdp->texture_tile[tile].masks;
                mGeometryDiagnostics.textureMaskT = mRdp->texture_tile[tile].maskt;
                mGeometryDiagnostics.textureScaleS = mRsp->texture_scaling_factor.s;
                mGeometryDiagnostics.textureScaleT = mRsp->texture_scaling_factor.t;
            }

            uint32_t tex_width1 = tex_width[i] << (cms & G_TX_MIRROR);
            uint32_t tex_height1 = tex_height[i] << (cmt & G_TX_MIRROR);

            if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_width1 != tex_width2[i])) {
                tm |= 1 << 2 * i;
                cms &= ~G_TX_CLAMP;
            }
            if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || tex_height1 != tex_height2[i])) {
                tm |= 1 << 2 * i + 1;
                cmt &= ~G_TX_CLAMP;
            }

            if (mRenderingState.mTextures[i] == nullptr) {
                /* Log draws that skip sampler setup because the GPU texture handle is null
                   despite TMEM holding real bytes. */
                static const bool sDiagFontMachineA = std::getenv("GDX_DIAG_FONT_MACHINE") != nullptr;
                if (sDiagFontMachineA) {
                    static int sNullTexLogs = 0;
                    if (sNullTexLogs < 32) {
                        ++sNullTexLogs;
                        gdx_dbg_logf("[fontmach] NULL mTextures[%d] tile=%u tmem=0x%X fmt=%u siz=%u\n",
                                      i, (unsigned)(i == 0 ? mRdp->first_tile_index
                                                           : ((mRdp->first_tile_index + 1) & 7)),
                                      mRdp->texture_tile[i == 0 ? mRdp->first_tile_index
                                                                : ((mRdp->first_tile_index + 1) & 7)].tmem_index,
                                      mRdp->texture_tile[i == 0 ? mRdp->first_tile_index
                                                                : ((mRdp->first_tile_index + 1) & 7)].fmt,
                                      mRdp->texture_tile[i == 0 ? mRdp->first_tile_index
                                                                : ((mRdp->first_tile_index + 1) & 7)].siz);
                    }
                }
                continue;
            }

            // N64 G_TF_AVERAGE is defined only for aligned 1:1 copies and samples exact texels there;
            // treating it as linear smears 1px HUD detail (e.g. minimap border) at upscaled resolutions.
            bool linear_filter = (mRdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BILERP;
            if (!mRenderingState.sampler_valid[i] ||
                linear_filter != mRenderingState.sampler_linear_filter[i] ||
                cms != mRenderingState.sampler_cms[i] || cmt != mRenderingState.sampler_cmt[i]) {
                Flush();

                // Set the same sampler params on the blended texture. Needed for opengl.
                if (activeLoaded.blended) {
                    mRapi->SetSamplerParameters(SHADER_FIRST_REPLACEMENT_TEXTURE + i, linear_filter, cms, cmt);
                }

                mRapi->SetSamplerParameters(i, linear_filter, cms, cmt);
                mRenderingState.sampler_valid[i] = true;
                mRenderingState.sampler_linear_filter[i] = linear_filter;
                mRenderingState.sampler_cms[i] = cms;
                mRenderingState.sampler_cmt[i] = cmt;
            }
        }
    }
    // [perf-s7] slow path taken: bank the tile-extent derivation and mark the state clean.
    // Clearing the dirty bit here (after imports/samplers ran) is what lets the next
    // same-state draw take the fast path above. Rect draws bank into their own instance
    // ([texrect] sGfxS7RectMemo) — never into the tri memo, whose exclusion rationale stands:
    // GfxDrawRectangle wraps its GfxSpTri1 calls in temporary othermode/viewport/geometry
    // mutations that it restores afterwards, which would leave a stale TRI memo marked clean
    // (and GfxDrawRectangle still invalidates the tri memo on exit).
    {
        s7Memo.comb = comb;
        s7Memo.tm = tm;
        for (int i = 0; i < 2; i++) {
            s7Memo.tex_width[i] = tex_width[i];
            s7Memo.tex_height[i] = tex_height[i];
            s7Memo.tex_width2[i] = tex_width2[i];
            s7Memo.tex_height2[i] = tex_height2[i];
            s7Memo.effective_tile[i] = effective_tile[i];
        }
        s7Memo.first_tile = mRdp->first_tile_index;
        s7Memo.valid = true;
        sGfxS7DirtyMask &= static_cast<uint8_t>(~s7DirtyBit);
        if (s7VerifyNewHit) {
            gdx_trifast_stat[kGdxTfHit]++; // a hit of the new predicate, re-derived for checking
            gdx_trifast_stat[kGdxTfHitNew]++;
            bool same = s7VerifyBank.tm == tm;
            for (int i = 0; i < 2; i++) {
                same = same && s7VerifyBank.tex_width[i] == tex_width[i] &&
                       s7VerifyBank.tex_height[i] == tex_height[i] &&
                       s7VerifyBank.tex_width2[i] == tex_width2[i] &&
                       s7VerifyBank.tex_height2[i] == tex_height2[i] &&
                       s7VerifyBank.effective_tile[i] == effective_tile[i];
            }
            if (!same) {
                gdx_trifast_stat[kGdxTfVfyMemo]++;
            }
        }
    }
    }
    GDX_TRI2_LAP(Memo);

    if (!is_rect) {
        mGeometryDiagnostics.lastShaderId0 = comb->shader_id0;
        mGeometryDiagnostics.lastShaderId1 = comb->shader_id1;
        if (originalUseFog) {
            mGeometryDiagnostics.fogTriangles++;
            // [perf-s7] range scan is diagnostics-only; the counter above stays unconditional.
            if (gdx_s7_geo_diag_enabled) {
                for (const LoadedVertex* vertex : v_arr) {
                    const float fogFactor =
                        use_blend_color ? mRdp->fog_color.a / 255.0f : vertex->color.a / 255.0f;
                    mGeometryDiagnostics.minFogFactor =
                        std::min(mGeometryDiagnostics.minFogFactor, fogFactor);
                    mGeometryDiagnostics.maxFogFactor =
                        std::max(mGeometryDiagnostics.maxFogFactor, fogFactor);
                }
            }
        }
        if (bypassFog) {
            mGeometryDiagnostics.fogBypassTriangles++;
        }
        if (comb->usedTextures[0] || comb->usedTextures[1]) {
            mGeometryDiagnostics.texturedTriangles++;
        }
        if (comb->usedTextures[0]) {
            if (mRenderingState.mTextures[0] != nullptr) {
                mGeometryDiagnostics.texture0BoundTriangles++;
            } else {
                mGeometryDiagnostics.texture0MissingTriangles++;
            }
        }
        if (forceSimpleMaterial) {
            mGeometryDiagnostics.forcedSimpleMaterialTriangles++;
        }
    }

    struct ShaderProgram* prg = comb->prg[tm];
    if (prg == NULL) {
        comb->prg[tm] = prg =
            LookupOrCreateShaderProgram(comb->shader_id0, comb->shader_id1 | tm * SHADER_OPT(TEXEL0_CLAMP_S));
    }
    if (prg != mRenderingState.mShaderProgram) {
        Flush();
        mRapi->UnloadShader(mRenderingState.mShaderProgram);
        mRapi->LoadShader(prg);
        mRenderingState.mShaderProgram = prg;
    }
    if (use_alpha != mRenderingState.alpha_blend) {
        Flush();
        mRapi->SetUseAlpha(use_alpha);
        mRenderingState.alpha_blend = use_alpha;
    }
    uint8_t numInputs;
    bool usedTextures[2];
    struct GfxClipParameters clip_parameters;

    // [perf-s7] Both calls are virtual round trips into the backend that only depend on prg and
    // the bound framebuffer, and every opcode that can switch either invalidates the memo.
    // [texrect] Rect draws bank/consume via their own instance (s7Memo aliases it above).
    if (s7TexMemoHit && !s7VerifyNewHit && s7Memo.prg == static_cast<const void*>(prg)) {
        numInputs = s7Memo.numInputs;
        usedTextures[0] = s7Memo.usedTextures[0];
        usedTextures[1] = s7Memo.usedTextures[1];
        clip_parameters = s7Memo.clip_parameters;
    } else {
        mRapi->ShaderGetInfo(prg, &numInputs, usedTextures);
        clip_parameters = mRapi->GetClipParameters();
        s7Memo.prg = prg;
        s7Memo.numInputs = numInputs;
        s7Memo.usedTextures[0] = usedTextures[0];
        s7Memo.usedTextures[1] = usedTextures[1];
        s7Memo.clip_parameters = clip_parameters;
    }
    GDX_TRI2_LAP(Prg);

    // [pit-uv-probe]: the PIT strip imports and draws with correct state yet renders
    // flat tan. Log per-vertex (s,t) plus the tile window for tiles 1-4 so a flat
    // draw shows whether the UVs collapsed or landed outside the window.
    static int sPitUvProbeTris = 0;
    const bool pitUvProbeThisTri = gGdxRaceActive != 0 && !is_rect && sPitUvProbeTris < 24 &&
                                    effective_tile[0] >= 1 && effective_tile[0] <= 4;
    if (pitUvProbeThisTri) {
        ++sPitUvProbeTris;
    }

#ifdef __3DS__
    // [triloop] Fold the backend repack into the first write (precedent:
    // Wyatt-James/sm64-3ds-port tri-loop optimization, ~0.8 ms measured there).
    // Legacy shape: this loop appended a VARIABLE-stride vertex (pos + uv +
    // per-vertex clamp limits + fog rgba + grayscale + EVERY combiner input) to
    // mBufVbo, then DrawTriangles re-walked the whole batch repacking it into
    // the fixed 12-float PICA layout — every vertex copied twice, with the
    // per-vertex work for constant-spill inputs (prim/env/key/convert) computed
    // and then discarded down to a single vertex-0 read.
    //
    // Packed shape: the batch's first append latches the backend pack
    // parameters (gdx3ds_vbopack_begin — refusal falls back to the legacy path
    // for the whole batch) and each vertex is written ONCE, in final layout,
    // straight into the backend's linear VBO pool: UV scale + both clamp
    // families applied here, only the vtxColorInput computed per vertex, and
    // the constant-spill inputs computed once per batch into the aux record
    // (sGdxPackAux) that replaces the backend's vertex-0 reads. All values are
    // bit-identical to legacy-append-then-repack by construction (same
    // expressions, same float op order); the flush invariant (any pack-input
    // state change splits the batch) makes batch-begin latching exact.
    //
    // gdx_s7_geo_diag_enabled forces legacy: the verbose geometry diagnostics
    // ([geodiag]/[pit-uv-probe]/uvprobe UV ranges) live in the legacy loop, so
    // armed diagnostics keep full fidelity. Killswitch: [debug] triloop=0.
    if (gdx3ds_verify_mode != 0) { /* [nverify] diagnostic, inline-gated: see gdx3ds_verify.h */
        Gdx3dsVerifyTri vt;
        memset(&vt, 0, sizeof(vt));
        const auto& t0 = mRdp->texture_tile[effective_tile[0]];
        const auto& l0 = mRdp->LoadedTextureAt(t0.tmem_index);
        vt.tile0 = effective_tile[0]; vt.tile1 = effective_tile[1]; vt.firstTile = mRdp->first_tile_index; vt.isRect = is_rect;
        vt.uls0 = t0.uls; vt.ult0 = t0.ult; vt.lrs0 = t0.lrs; vt.lrt0 = t0.lrt;
        vt.texW0 = tex_width[0]; vt.texH0 = tex_height[0]; vt.texW20 = tex_width2[0]; vt.texH20 = tex_height2[0];
        vt.texW1 = tex_width[1]; vt.texH1 = tex_height[1]; vt.tm = tm; vt.vu = v1->u; vt.vv = v1->v;
        vt.shifts0 = t0.shifts; vt.shiftt0 = t0.shiftt; vt.line0 = l0.line_size_bytes; vt.size0 = l0.size_bytes;
        vt.full0 = l0.full_image_line_size_bytes; vt.orig0 = l0.orig_size_bytes;
        vt.scaleS = mRsp->texture_scaling_factor.s; vt.scaleT = mRsp->texture_scaling_factor.t;
        vt.omh = mRdp->other_mode_h; vt.oml = mRdp->other_mode_l; vt.geo = mRsp->geometry_mode;
        vt.variant = (unsigned)mF3dex2Variant;
        vt.vpx = mRdp->viewport.x; vt.vpy = mRdp->viewport.y; vt.vpw = mRdp->viewport.width; vt.vph = mRdp->viewport.height;
        vt.blendA = mRdp->blend_color.a;
        {
            const uint32_t tmemOff = static_cast<uint32_t>(t0.tmem_index) * 8u;
            vt.used0 = comb->usedTextures[0] ? 1u : 0u; vt.used1 = comb->usedTextures[1] ? 1u : 0u;
            vt.tmem0 = t0.tmem_index;
            vt.rtLine0 = t0.line_size_bytes;
            if (t0.tmem_index < 512 && mRdp->tmem_span_base[t0.tmem_index] != 0) {
                vt.base0 = mRdp->tmem_span_base[t0.tmem_index] - 1u;
                const auto& br = mRdp->loaded_texture[vt.base0];
                vt.baseAddr0 = reinterpret_cast<unsigned long>(br.addr); vt.baseSize0 = br.size_bytes; vt.baseOrig0 = br.orig_size_bytes;
            }
            /* [nverify] fmt/siz/palette of the render tile + the live palette staging, so a
             * texture-identity mismatch names the format and says whether the TLUT moved. */
            vt.tc1 = ((unsigned)t0.fmt << 8) | (unsigned)t0.siz | ((unsigned)t0.palette << 16) |
                     (l0.raw_tex_metadata.resource != nullptr ? 0x1000000u : 0u);
            /* CI only, and only the palette half the decode reads: this is per-triangle. */
            vt.tc0 = 0;
            if (t0.fmt == G_IM_FMT_CI && (t0.siz == G_IM_SIZ_4b || t0.siz == G_IM_SIZ_8b)) {
                uint32_t ph = 2166136261u;
                if (t0.siz == G_IM_SIZ_4b) {
                    const uint8_t* pal = mRdp->palette_staging[(t0.palette / 8) & 1] + (t0.palette % 8) * 32u;
                    for (unsigned b = 0; b < 32u; b++) ph = (ph ^ pal[b]) * 16777619u;
                } else {
                    for (unsigned h = 0; h < 2u; h++) {
                        for (unsigned b = 0; b < 256u; b++) ph = (ph ^ mRdp->palette_staging[h][b]) * 16777619u;
                    }
                }
                vt.tc0 = ph;
            }
            /* The TMEM bytes the load actually occupies: size_bytes counts SOURCE bytes, while
               LOADTILE writes each row at an 8-byte-aligned destination stride, so for a 74-byte
               row the footprint is 80 per row and the tile's last rows sit BEYOND size_bytes --
               which is where the decode reads. Must match gdx_tri.cpp's VfyTmemFootprint. */
            const unsigned tmemFoot0 = GdxVfyTmemFootprint(l0.size_bytes, l0.line_size_bytes);
            const unsigned tmemSpan0 =
                tmemOff < sizeof(mRdp->tmem)
                    ? (unsigned)std::min<size_t>(sizeof(mRdp->tmem) - tmemOff, std::max<unsigned>(tmemFoot0, 8u))
                    : 0u;
            gdx3ds_verify_tri_source(&vt, vt.used0 ? l0.addr : nullptr, l0.size_bytes,
                                     tmemSpan0 != 0 ? mRdp->tmem + tmemOff : nullptr, tmemSpan0);
            /* LoadedTextureAt materializes a lazy span view (bookkeeping side effects): only touch the
             * unit-1 slot when the combiner actually samples it, exactly like the import loop. */
            if (vt.used1) {
                const auto& t1 = mRdp->texture_tile[effective_tile[1]];
                const auto& l1 = mRdp->LoadedTextureAt(t1.tmem_index);
                const uint32_t tmemOff1 = static_cast<uint32_t>(t1.tmem_index) * 8u;
                const unsigned tmemSpan1 =
                    tmemOff1 < sizeof(mRdp->tmem)
                        ? (unsigned)std::min<size_t>(sizeof(mRdp->tmem) - tmemOff1, std::max<uint32_t>(l1.size_bytes, 8u))
                        : 0u;
                gdx3ds_verify_tri_source1(&vt, l1.addr, l1.size_bytes,
                                          tmemSpan1 != 0 ? mRdp->tmem + tmemOff1 : nullptr, tmemSpan1);
                vt.tmem1 = t1.tmem_index; vt.line1 = l1.line_size_bytes; vt.size1 = l1.size_bytes; vt.full1 = l1.full_image_line_size_bytes;
                vt.orig1 = l1.orig_size_bytes;
            }
        }
        gdx3ds_verify_note_tri(&vt);
    }
    if (mBufVboNumTris == 0) {
        sGdxPackDst = gdx_s7_geo_diag_enabled ? nullptr : gdx3ds_vbopack_begin(&sGdxPackParams);
        sGdxPackBatch = (sGdxPackDst != nullptr);
        sGdxPackComb = comb; // [native] fx_hud fold key
        GDX_TRI2_CNT(Batch);
        GDX_TRI2_LAP(Begin);
    }
    if (sGdxPackBatch) {
        GDX_TRI2_CNT(Packed);
        // Combiner-input evaluation, verbatim from the legacy loop's switch (the
        // uint8 RGBA truncations included). isShade flags the SHADE case for the
        // standard-fog alpha substitution (legacy: color == &v_arr[i]->color).
        struct GdxPackInput {
            RGBA c;
            bool isShade;
        };
        const auto gdxInput = [&](int j, int k, int i) -> GdxPackInput {
            RGBA* color;
            RGBA tmp;
            bool isShade = false;
            switch (comb->shader_input_mapping[k][j]) {
                case G_CCMUX_PRIMITIVE:
                    color = &mRdp->prim_color;
                    break;
                case G_CCMUX_SHADE:
                    color = &v_arr[i]->color;
                    isShade = true;
                    break;
                case G_CCMUX_SHADE_ALPHA: {
                    tmp.r = tmp.g = tmp.b = tmp.a = v_arr[i]->color.a;
                    color = &tmp;
                    break;
                }
                case G_CCMUX_ENVIRONMENT:
                    color = &mRdp->env_color;
                    break;
                case G_CCMUX_PRIMITIVE_ALPHA: {
                    tmp.r = tmp.g = tmp.b = mRdp->prim_color.a;
                    color = &tmp;
                    break;
                }
                case G_CCMUX_ENV_ALPHA: {
                    tmp.r = tmp.g = tmp.b = mRdp->env_color.a;
                    color = &tmp;
                    break;
                }
                case G_CCMUX_PRIM_LOD_FRAC: {
                    tmp.r = tmp.g = tmp.b = mRdp->prim_lod_fraction;
                    color = &tmp;
                    break;
                }
                case G_CCMUX_LOD_FRACTION: {
                    if (mRdp->other_mode_l & G_TL_LOD) {
                        // "Hack" that works for Bowser - Peach painting
                        float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                        if (distance_frac < 0.0f) {
                            distance_frac = 0.0f;
                        }
                        if (distance_frac > 1.0f) {
                            distance_frac = 1.0f;
                        }
                        tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
                    } else {
                        tmp.r = tmp.g = tmp.b = tmp.a = 255.0f;
                    }
                    color = &tmp;
                    break;
                }
                case G_CCMUX_KEY_CENTER:
                    color = &mRdp->key_center;
                    break;
                case G_CCMUX_KEY_SCALE:
                    color = &mRdp->key_scale;
                    break;
                case G_CCMUX_CONVERT_K4: {
                    tmp.r = tmp.g = tmp.b = mRdp->convert_k[4];
                    color = &tmp;
                    break;
                }
                case G_CCMUX_CONVERT_K5: {
                    tmp.r = tmp.g = tmp.b = mRdp->convert_k[5];
                    color = &tmp;
                    break;
                }
                case G_ACMUX_PRIM_LOD_FRAC:
                    tmp.a = mRdp->prim_lod_fraction;
                    color = &tmp;
                    break;
                default:
                    memset(&tmp, 0, sizeof(tmp));
                    color = &tmp;
                    break;
            }
            return { *color, isShade };
        };

        // Final clamp limits, resolved once per tri from this draw's tm bits and
        // the batch pack parameters. Shader-clamp axes carry the interpreter's
        // own limit ((tileExtent - 0.5) / texExtent — the value the legacy path
        // appended per vertex) scaled into padded UV space; other axes use the
        // backend's content-edge limit. Both are what DrawTriangles' repack
        // derived, gated identically on the sky_clamp_fix flag.
        float gdxClampU[2] = { 1e30f, 1e30f };
        float gdxClampV[2] = { 1e30f, 1e30f };
        for (int t = 0; t < 2; t++) {
            if (!usedTextures[t]) {
                continue;
            }
            gdxClampU[t] = sGdxPackParams.wrapClampU[t];
            gdxClampV[t] = sGdxPackParams.wrapClampV[t];
            if (sGdxPackParams.shaderClampGate) {
                if (tm & (1 << 2 * t)) {
                    gdxClampU[t] = ((tex_width2[t] - 0.5f) / tex_width[t]) * sGdxPackParams.uScale[t];
                }
                if (tm & (1 << 2 * t + 1)) {
                    gdxClampV[t] = ((tex_height2[t] - 0.5f) / tex_height[t]) * sGdxPackParams.vScale[t];
                }
            }
        }
        GDX_TRI2_LAP(Pack);

        // Batch aux record (first tri only): the vertex-0 value of every
        // combiner input plus the draw-constant fog rgb/factor — everything the
        // lossy fixed layout no longer carries but the backend's vertex-0 reads
        // consumed (RefreshStageConstants, UpdateFogState).
        if (mBufVboNumTris == 0) {
            sGdxPackAux.numInputs = (uint8_t)(numInputs < 7 ? numInputs : 7);
            for (int j = 0; j < sGdxPackAux.numInputs; j++) {
                const GdxPackInput cv = gdxInput(j, 0, 0);
                sGdxPackAux.inputRgba[j][0] = cv.c.r / 255.0f;
                sGdxPackAux.inputRgba[j][1] = cv.c.g / 255.0f;
                sGdxPackAux.inputRgba[j][2] = cv.c.b / 255.0f;
                if (use_alpha) {
                    const GdxPackInput av = gdxInput(j, 1, 0);
                    sGdxPackAux.inputRgba[j][3] =
                        (use_fog && !use_blend_color && av.isShade) ? 1.0f : av.c.a / 255.0f;
                } else {
                    sGdxPackAux.inputRgba[j][3] = 1.0f;
                }
            }
            sGdxPackAux.hasFog = use_fog ? 1 : 0;
            if (use_fog) {
                const RGBA& fogSrc = use_blend_color ? mRdp->blend_color : mRdp->fog_color;
                sGdxPackAux.fogRgb[0] = fogSrc.r / 255.0f;
                sGdxPackAux.fogRgb[1] = fogSrc.g / 255.0f;
                sGdxPackAux.fogRgb[2] = fogSrc.b / 255.0f;
                sGdxPackAux.fogFactor0 =
                    use_blend_color ? mRdp->fog_color.a / 255.0f : v_arr[0]->color.a / 255.0f;
            } else {
                sGdxPackAux.fogRgb[0] = sGdxPackAux.fogRgb[1] = sGdxPackAux.fogRgb[2] = 0.0f;
                sGdxPackAux.fogFactor0 = 0.0f;
            }
            GDX_TRI2_LAP(Begin);
        }

        float* gdxOut = sGdxPackDst + (size_t)mBufVboNumTris * (3 * GDX3DS_VBOPACK_STRIDE);
        if (sGdxTriFast) {
            // [trifast] Same emission as the loop below with the per-vertex divides, library
            // calls and mux switches lifted out (each substitution bit-identical, see the
            // gdx_trifast_stat block comment). Per-tri prep, then three straight-line vertices.
            gdx_trifast_stat[kGdxTfFastTris]++;
            const bool bilerp = (mRdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BILERP;
            const bool halfTexel = bilerp && (!is_rect || !sGdxRectHalfTexelDisabled);
            struct GdxUvPrep {
                float uOff, vOff;      // uls/4, ult/4
                float uPre, vPre;      // 2^-shift for shifts in 1..10 (exact), else 2^(16-shift)
                float uNormMul, vNormMul; // exact reciprocal when the extent is a power of two
                float uNormDiv, vNormDiv; // (float)extent for the divide fallback
                bool uMul, vMul;
            } prep[2];
            for (int t = 0; t < 2; t++) {
                if (!usedTextures[t]) {
                    continue;
                }
                const uint32_t uv_tile = effective_tile[t];
                const int shifts = mRdp->texture_tile[uv_tile].shifts;
                const int shiftt = mRdp->texture_tile[uv_tile].shiftt;
                // Legacy: `u /= 1 << shifts` for 1..10 — a divide by 2^k, which equals a
                // multiply by 2^-k exactly; `u *= 1 << (16 - shifts)` above 10 (unchanged).
                prep[t].uPre = shifts == 0 ? 1.0f : (shifts <= 10 ? 1.0f / (float)(1 << shifts) : (float)(1 << (16 - shifts)));
                prep[t].vPre = shiftt == 0 ? 1.0f : (shiftt <= 10 ? 1.0f / (float)(1 << shiftt) : (float)(1 << (16 - shiftt)));
                prep[t].uOff = mRdp->texture_tile[uv_tile].uls / 4.0f;
                prep[t].vOff = mRdp->texture_tile[uv_tile].ult / 4.0f;
                const uint32_t tw = tex_width[t];
                const uint32_t th = tex_height[t];
                prep[t].uMul = (tw & (tw - 1u)) == 0u;
                prep[t].vMul = (th & (th - 1u)) == 0u;
                prep[t].uNormDiv = (float)tw;
                prep[t].vNormDiv = (float)th;
                prep[t].uNormMul = prep[t].uMul ? 1.0f / (float)tw : 1.0f;
                prep[t].vNormMul = prep[t].vMul ? 1.0f / (float)th : 1.0f;
                gdx_trifast_stat[prep[t].uMul ? kGdxTfUvMul : kGdxTfUvDiv] += 3;
            }
            // Combiner input muxes are per-tri constants; the SHADE case (the race norm) reads
            // the vertex directly, anything else goes through the verbatim lambda.
            const int ci = sGdxPackParams.vtxColorInput - 1;
            const bool colorIsShade = ci >= 0 && comb->shader_input_mapping[0][ci] == G_CCMUX_SHADE;
            const int ai = sGdxPackParams.vtxAlphaInput - 1;
            const bool alphaIsShade = ai >= 0 && comb->shader_input_mapping[1][ai] == G_CCMUX_SHADE;
            const bool fogFactorConst = sGdxPackParams.fogFactorToAlpha && use_blend_color;
            const float fogFactorConstVal = fogFactorConst ? sGdxByteToUnit.v[mRdp->fog_color.a] : 0.0f;
            const bool shadeAlphaIsOne = use_fog && !use_blend_color;
            for (int i = 0; i < 3; i++) {
                float* out = gdxOut + i * GDX3DS_VBOPACK_STRIDE;
                const LoadedVertex* vv = v_arr[i];
                float z = vv->z, w = vv->w;
                if (clip_parameters.z_is_from_0_to_1) {
                    z = (z + w) / 2.0f;
                }
                out[0] = vv->x;
                out[1] = clip_parameters.invertY ? -vv->y : vv->y;
                out[2] = z;
                out[3] = w;
                for (int t = 0; t < 2; t++) {
                    const int slot = (t == 0) ? 4 : 10;
                    if (!usedTextures[t]) {
                        out[slot] = 0.0f;
                        out[slot + 1] = 0.0f;
                        continue;
                    }
                    float u = vv->u / 32.0f;
                    float v = vv->v / 32.0f;
                    u *= prep[t].uPre;
                    v *= prep[t].vPre;
                    u -= prep[t].uOff;
                    v -= prep[t].vOff;
                    if (halfTexel) {
                        u += 0.5f;
                        v += 0.5f;
                    }
                    const float nu = prep[t].uMul ? u * prep[t].uNormMul : u / prep[t].uNormDiv;
                    const float nv = prep[t].vMul ? v * prep[t].vNormMul : v / prep[t].vNormDiv;
                    // [trectbatch] atlas view: mirror the legacy loop exactly -- the page
                    // offset rides AFTER the clamp so the limits stay in view space.
                    const float pu = GdxFminExact(nu * sGdxPackParams.uScale[t], gdxClampU[t]);
                    const float pv = GdxFminExact(nv * sGdxPackParams.vScale[t], gdxClampV[t]);
                    if (sGdxPackParams.hasOff) {
                        out[slot] = pu + sGdxPackParams.uOff[t];
                        out[slot + 1] = pv + sGdxPackParams.vOff[t];
                    } else {
                        out[slot] = pu;
                        out[slot + 1] = pv;
                    }
                }
                if (ci >= 0) {
                    if (colorIsShade) {
                        out[6] = sGdxByteToUnit.v[vv->color.r];
                        out[7] = sGdxByteToUnit.v[vv->color.g];
                        out[8] = sGdxByteToUnit.v[vv->color.b];
                    } else {
                        const GdxPackInput cv = gdxInput(ci, 0, i);
                        out[6] = sGdxByteToUnit.v[cv.c.r];
                        out[7] = sGdxByteToUnit.v[cv.c.g];
                        out[8] = sGdxByteToUnit.v[cv.c.b];
                    }
                } else {
                    out[6] = out[7] = out[8] = 1.0f;
                }
                if (sGdxPackParams.fogFactorToAlpha) {
                    out[9] = fogFactorConst ? fogFactorConstVal : sGdxByteToUnit.v[vv->color.a];
                } else if (sGdxPackParams.wantVtxAlpha) {
                    if (alphaIsShade) {
                        out[9] = shadeAlphaIsOne ? 1.0f : sGdxByteToUnit.v[vv->color.a];
                    } else {
                        const GdxPackInput av = gdxInput(ai, 1, i);
                        out[9] = (shadeAlphaIsOne && av.isShade) ? 1.0f : sGdxByteToUnit.v[av.c.a];
                    }
                } else {
                    out[9] = 1.0f;
                }
            }
            if (use_fog) {
                if (use_blend_color) {
                    gdx3ds_fog_note_params(0, 0, 0);
                } else {
                    gdx3ds_fog_note_params(mRsp->fog_mul, mRsp->fog_offset, 1);
                }
            }
        }
        float gdxVerifyScratch[3 * GDX3DS_VBOPACK_STRIDE];
        float* const gdxLegacyBase = sGdxTriFast ? gdxVerifyScratch : gdxOut;
        if (!sGdxTriFast || sGdxTriFastVerify) {
        for (int i = 0; i < 3; i++) {
            float* out = gdxLegacyBase + i * GDX3DS_VBOPACK_STRIDE;
            float z = v_arr[i]->z, w = v_arr[i]->w;
            if (clip_parameters.z_is_from_0_to_1) {
                z = (z + w) / 2.0f;
            }
            out[0] = v_arr[i]->x;
            out[1] = clip_parameters.invertY ? -v_arr[i]->y : v_arr[i]->y;
            out[2] = z;
            out[3] = w;

            for (int t = 0; t < 2; t++) {
                const int slot = (t == 0) ? 4 : 10;
                if (!usedTextures[t]) {
                    out[slot] = 0.0f;
                    out[slot + 1] = 0.0f;
                    continue;
                }
                float u = v_arr[i]->u / 32.0f;
                float v = v_arr[i]->v / 32.0f;

                const uint32_t uv_tile = effective_tile[t];
                const int shifts = mRdp->texture_tile[uv_tile].shifts;
                const int shiftt = mRdp->texture_tile[uv_tile].shiftt;
                if (shifts != 0) {
                    if (shifts <= 10) {
                        u /= 1 << shifts;
                    } else {
                        u *= 1 << (16 - shifts);
                    }
                }
                if (shiftt != 0) {
                    if (shiftt <= 10) {
                        v /= 1 << shiftt;
                    } else {
                        v *= 1 << (16 - shiftt);
                    }
                }

                u -= mRdp->texture_tile[uv_tile].uls / 4.0f;
                v -= mRdp->texture_tile[uv_tile].ult / 4.0f;

                // Must agree with the sampler filter policy (only G_TF_BILERP is linear).
                if ((mRdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BILERP) {
                    if (!is_rect || !sGdxRectHalfTexelDisabled) {
                        u += 0.5f;
                        v += 0.5f;
                    }
                }

                // normalized UV * pow2-padding scale, clamped — the exact repack
                // expression (fminf keeps the NaN policy identical).
                if (sGdxPackParams.hasOff) {
                    // [trectbatch] atlas view: the page offset rides AFTER the clamp so the
                    // limits stay in the view's own (un-offset) space.
                    out[slot] = fminf((u / tex_width[t]) * sGdxPackParams.uScale[t], gdxClampU[t]) +
                                sGdxPackParams.uOff[t];
                    out[slot + 1] = fminf((v / tex_height[t]) * sGdxPackParams.vScale[t], gdxClampV[t]) +
                                    sGdxPackParams.vOff[t];
                } else {
                    out[slot] = fminf((u / tex_width[t]) * sGdxPackParams.uScale[t], gdxClampU[t]);
                    out[slot + 1] = fminf((v / tex_height[t]) * sGdxPackParams.vScale[t], gdxClampV[t]);
                }
            }

            if (sGdxPackParams.vtxColorInput > 0) {
                const GdxPackInput cv = gdxInput(sGdxPackParams.vtxColorInput - 1, 0, i);
                out[6] = cv.c.r / 255.0f;
                out[7] = cv.c.g / 255.0f;
                out[8] = cv.c.b / 255.0f;
            } else {
                out[6] = out[7] = out[8] = 1.0f;
            }

            // out[9] policy mirrors the repack: fog factor for fog-blend draws
            // (fog slot +3: constant fog alpha in shroud mode, baked per-vertex
            // factor otherwise), else the vtxAlphaInput's per-vertex alpha with
            // the standard-fog shade substitution, else opaque.
            if (sGdxPackParams.fogFactorToAlpha) {
                out[9] = use_blend_color ? mRdp->fog_color.a / 255.0f : v_arr[i]->color.a / 255.0f;
            } else if (sGdxPackParams.wantVtxAlpha) {
                const GdxPackInput av = gdxInput(sGdxPackParams.vtxAlphaInput - 1, 1, i);
                out[9] = (use_fog && !use_blend_color && av.isShade) ? 1.0f : av.c.a / 255.0f;
            } else {
                out[9] = 1.0f;
            }

            // Fog-line mirror, same cadence as the legacy append (the moveword
            // handlers flush on a line change, so the latch is batch-exact).
            if (use_fog) {
                if (use_blend_color) {
                    gdx3ds_fog_note_params(0, 0, 0); // constant factor: backend must fit, not use the line
                } else {
                    gdx3ds_fog_note_params(mRsp->fog_mul, mRsp->fog_offset, 1);
                }
            }
        }
        } // [trifast] legacy packed loop (verify mode: into scratch)
        if (sGdxTriFastVerify && memcmp(gdxOut, gdxVerifyScratch, sizeof(gdxVerifyScratch)) != 0) {
            gdx_trifast_stat[kGdxTfVfyPack]++;
        }
        mBufVboLen += 3 * GDX3DS_VBOPACK_STRIDE;
        GDX_TRI2_LAP(Pack);
    } else {
    GDX_TRI2_CNT(Legacy);
#endif
    for (int i = 0; i < 3; i++) {
        float z = v_arr[i]->z, w = v_arr[i]->w;
        if (clip_parameters.z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }

        mBufVbo[mBufVboLen++] = v_arr[i]->x;
        mBufVbo[mBufVboLen++] = clip_parameters.invertY ? -v_arr[i]->y : v_arr[i]->y;
        mBufVbo[mBufVboLen++] = z;
        mBufVbo[mBufVboLen++] = w;

        for (int t = 0; t < 2; t++) {
            if (!usedTextures[t]) {
                continue;
            }
            float u = v_arr[i]->u / 32.0f;
            float v = v_arr[i]->v / 32.0f;

            uint32_t uv_tile = effective_tile[t];
            int shifts = mRdp->texture_tile[uv_tile].shifts;
            int shiftt = mRdp->texture_tile[uv_tile].shiftt;

            // Env-gated (GDX_DIAG_PITUV) so a normal Release run stays silent; cached once.
            static const bool sDiagPitUv = std::getenv("GDX_DIAG_PITUV") != nullptr;
            if (sDiagPitUv && pitUvProbeThisTri && t == 0) {
                SPDLOG_ERROR("[pit-uv-probe] tri={} vtx={} tile={} s={} t={} uls={} ult={} lrs={} lrt={} "
                             "shifts={} shiftt={}",
                             sPitUvProbeTris, i, uv_tile, (int)v_arr[i]->u, (int)v_arr[i]->v,
                             mRdp->texture_tile[uv_tile].uls, mRdp->texture_tile[uv_tile].ult,
                             mRdp->texture_tile[uv_tile].lrs, mRdp->texture_tile[uv_tile].lrt, shifts, shiftt);
            }

            if (shifts != 0) {
                if (shifts <= 10) {
                    u /= 1 << shifts;
                } else {
                    u *= 1 << (16 - shifts);
                }
            }
            if (shiftt != 0) {
                if (shiftt <= 10) {
                    v /= 1 << shiftt;
                } else {
                    v *= 1 << (16 - shiftt);
                }
            }

            u -= mRdp->texture_tile[uv_tile].uls / 4.0f;
            v -= mRdp->texture_tile[uv_tile].ult / 4.0f;

            // Must agree with the sampler filter policy above (only G_TF_BILERP is linear).
            if ((mRdp->other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BILERP) {
                // Linear filtering shifts an N64 texel index to a GPU texel centre. For rects the
                // offset is opt-in; see sGdxRectHalfTexelDisabled. File-scope flag rather than a
                // function-local static so this per-vertex path pays no init guard.
                if (!is_rect || !sGdxRectHalfTexelDisabled) {
                    u += 0.5f;
                    v += 0.5f;
                }
            }

            const float normalizedU = u / tex_width[t];
            const float normalizedV = v / tex_height[t];
            // An axis with CLAMP enabled should never receive coordinates several repeats outside
            // its tile window. Wrap-masked tiles legitimately do and no longer trigger.
            static const bool diagUvProbe = std::getenv("GDX_DIAG_UVPROBE") != nullptr;
            const uint8_t probeCms = mRdp->texture_tile[uv_tile].cms;
            const uint8_t probeCmt = mRdp->texture_tile[uv_tile].cmt;
            const bool clampAnomaly =
                (((probeCmt & G_TX_CLAMP) != 0) && (normalizedV < -2.0f || normalizedV > 2.0f)) ||
                (((probeCms & G_TX_CLAMP) != 0) && (normalizedU < -2.0f || normalizedU > 2.0f));
            if (diagUvProbe && gGdxRaceActive != 0 && !is_rect && t == 0 && clampAnomaly) {
                static int sUvProbe = 0;
                if (sUvProbe < 96) {
                    ++sUvProbe;
                    FILE* uf = fopen("uvprobe.txt", "a");
                    if (uf != nullptr) {
                        fprintf(uf,
                                "uv_tile=%u rawS=%d rawT=%d shifts=%d shiftt=%d "
                                "texW=%u texH=%u tileW2=%u tileH2=%u u=%.2f v=%.2f normU=%.2f normV=%.2f "
                                "cms=%u cmt=%u masks=%u maskt=%u size=[%u,%u..%u,%u] "
                                "scale=%04X/%04X line=%u tmem=0x%X fmt=%u siz=%u "
                                "addr=%p combine=%016llX geom=%08X othermodeH=%08X\n",
                                uv_tile, (int)v_arr[i]->u, (int)v_arr[i]->v, shifts, shiftt,
                                tex_width[t], tex_height[t], tex_width2[t], tex_height2[t],
                                u, v, normalizedU, normalizedV,
                                mRdp->texture_tile[uv_tile].cms, mRdp->texture_tile[uv_tile].cmt,
                                mRdp->texture_tile[uv_tile].masks, mRdp->texture_tile[uv_tile].maskt,
                                mRdp->texture_tile[uv_tile].uls, mRdp->texture_tile[uv_tile].ult,
                                mRdp->texture_tile[uv_tile].lrs, mRdp->texture_tile[uv_tile].lrt,
                                (unsigned)mRsp->texture_scaling_factor.s,
                                (unsigned)mRsp->texture_scaling_factor.t,
                                mRdp->texture_tile[uv_tile].line_size_bytes,
                                (unsigned)mRdp->texture_tile[uv_tile].tmem,
                                (unsigned)mRdp->texture_tile[uv_tile].fmt,
                                (unsigned)mRdp->texture_tile[uv_tile].siz,
                                (const void*)mRdp->LoadedTextureAt(mRdp->texture_tile[uv_tile].tmem_index).addr,
                                (unsigned long long)mRdp->combine_mode,
                                (unsigned)mRsp->geometry_mode,
                                (unsigned)mRdp->other_mode_h);
                        fclose(uf);
                    }
                    // Dump the SETTILE ring plus a draw marker so the tile-write
                    // history immediately preceding this suspect draw is visible.
                    if (std::getenv("GDX_DIAG_SETTILE") != nullptr) {
                        extern int gGdxTraceSeq;
                        extern std::deque<std::string> gGdxTileTraceRing;
                        static int sTraceDumps = 0;
                        if (sTraceDumps < 16) {
                            ++sTraceDumps;
                            FILE* tf = fopen("settile-trace.txt", "a");
                            if (tf != nullptr) {
                                fprintf(tf, "==== dump %d ====\n", sTraceDumps);
                                for (const std::string& lineStr : gGdxTileTraceRing) {
                                    fprintf(tf, "%s\n", lineStr.c_str());
                                }
                                fprintf(tf,
                                        "D %06d tile=%u normV=%.2f cms=%u cmt=%u masks=%u maskt=%u addr=%p "
                                        "size=[%u,%u..%u,%u] pos=(%.0f,%.0f,%.0f,%.0f)\n",
                                        ++gGdxTraceSeq, uv_tile, normalizedV,
                                        mRdp->texture_tile[uv_tile].cms, mRdp->texture_tile[uv_tile].cmt,
                                        mRdp->texture_tile[uv_tile].masks, mRdp->texture_tile[uv_tile].maskt,
                                        (const void*)mRdp->LoadedTextureAt(mRdp->texture_tile[uv_tile].tmem_index).addr,
                                        mRdp->texture_tile[uv_tile].uls, mRdp->texture_tile[uv_tile].ult,
                                        mRdp->texture_tile[uv_tile].lrs, mRdp->texture_tile[uv_tile].lrt,
                                        v_arr[i]->x, v_arr[i]->y, v_arr[i]->z, v_arr[i]->w);
                                fclose(tf);
                            }
                            gGdxTileTraceRing.clear();
                        }
                    }
                }
            }
            mBufVbo[mBufVboLen++] = normalizedU;
            mBufVbo[mBufVboLen++] = normalizedV;
            if (!is_rect && t == 0 && gdx_s7_geo_diag_enabled && std::isfinite(normalizedU) &&
                std::isfinite(normalizedV)) {
                mGeometryDiagnostics.minTextureU = std::min(mGeometryDiagnostics.minTextureU, normalizedU);
                mGeometryDiagnostics.maxTextureU = std::max(mGeometryDiagnostics.maxTextureU, normalizedU);
                mGeometryDiagnostics.minTextureV = std::min(mGeometryDiagnostics.minTextureV, normalizedV);
                mGeometryDiagnostics.maxTextureV = std::max(mGeometryDiagnostics.maxTextureV, normalizedV);
            }

            bool clampS = tm & (1 << 2 * t);
            bool clampT = tm & (1 << 2 * t + 1);

            if (clampS) {
                mBufVbo[mBufVboLen++] = (tex_width2[t] - 0.5f) / tex_width[t];
            }

            if (clampT) {
                mBufVbo[mBufVboLen++] = (tex_height2[t] - 0.5f) / tex_height[t];
            }
        }

        if (use_fog) {
            if (use_blend_color) {
                // Shroud/blend mode: blend toward blend_color using fog alpha as factor
                mBufVbo[mBufVboLen++] = mRdp->blend_color.r / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->blend_color.g / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->blend_color.b / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->fog_color.a / 255.0f;
#ifdef __3DS__
                gdx3ds_fog_note_params(0, 0, 0); // constant factor: backend must fit, not use the line
#endif
            } else {
                mBufVbo[mBufVboLen++] = mRdp->fog_color.r / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->fog_color.g / 255.0f;
                mBufVbo[mBufVboLen++] = mRdp->fog_color.b / 255.0f;
                mBufVbo[mBufVboLen++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
#ifdef __3DS__
                gdx3ds_fog_note_params(mRsp->fog_mul, mRsp->fog_offset, 1);
#endif
            }
        }

        if (use_grayscale) {
            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.r / 255.0f;
            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.g / 255.0f;
            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.b / 255.0f;
            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.a / 255.0f; // lerp interpolation factor (not alpha)
        }

        for (int j = 0; j < numInputs; j++) {
            RGBA* color;
            RGBA tmp;
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                switch (comb->shader_input_mapping[k][j]) {
                        // Note: CCMUX constants and ACMUX constants used here have same value, which is why this works
                        // (except LOD fraction).
                    case G_CCMUX_PRIMITIVE:
                        color = &mRdp->prim_color;
                        break;
                    case G_CCMUX_SHADE:
                        color = &v_arr[i]->color;
                        break;
                    case G_CCMUX_SHADE_ALPHA: {
                        // Per-vertex shade alpha broadcast to rgb (F-Zero X machine debris:
                        // rgb = lerp(SHADE, 1, SHADE_ALPHA) — the white-hot flicker factor).
                        tmp.r = tmp.g = tmp.b = v_arr[i]->color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_ENVIRONMENT:
                        color = &mRdp->env_color;
                        break;
                    case G_CCMUX_PRIMITIVE_ALPHA: {
                        tmp.r = tmp.g = tmp.b = mRdp->prim_color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_ENV_ALPHA: {
                        tmp.r = tmp.g = tmp.b = mRdp->env_color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_PRIM_LOD_FRAC: {
                        tmp.r = tmp.g = tmp.b = mRdp->prim_lod_fraction;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_LOD_FRACTION: {
                        if (mRdp->other_mode_l & G_TL_LOD) {
                            // "Hack" that works for Bowser - Peach painting
                            float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                            if (distance_frac < 0.0f) {
                                distance_frac = 0.0f;
                            }
                            if (distance_frac > 1.0f) {
                                distance_frac = 1.0f;
                            }
                            tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
                        } else {
                            tmp.r = tmp.g = tmp.b = tmp.a = 255.0f;
                        }
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_KEY_CENTER:
                        color = &mRdp->key_center;
                        break;
                    case G_CCMUX_KEY_SCALE:
                        color = &mRdp->key_scale;
                        break;
                    case G_CCMUX_CONVERT_K4: {
                        tmp.r = tmp.g = tmp.b = mRdp->convert_k[4];
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_CONVERT_K5: {
                        tmp.r = tmp.g = tmp.b = mRdp->convert_k[5];
                        color = &tmp;
                        break;
                    }
                    case G_ACMUX_PRIM_LOD_FRAC:
                        tmp.a = mRdp->prim_lod_fraction;
                        color = &tmp;
                        break;
                    default:
                        memset(&tmp, 0, sizeof(tmp));
                        color = &tmp;
                        break;
                }
                if (k == 0) {
                    mBufVbo[mBufVboLen++] = color->r / 255.0f;
                    mBufVbo[mBufVboLen++] = color->g / 255.0f;
                    mBufVbo[mBufVboLen++] = color->b / 255.0f;
                } else {
                    if (use_fog && !use_blend_color && color == &v_arr[i]->color) {
                        // Shade alpha is 100% for standard fog, blend color mode preserves
                        // it since fog alpha is the blend factor
                        mBufVbo[mBufVboLen++] = 1.0f;
                    } else {
                        mBufVbo[mBufVboLen++] = color->a / 255.0f;
                    }
                }
            }
        }

        // struct RGBA *color = &v_arr[i]->color;
        // mBufVbo[mBufVboLen++] = color->r / 255.0f;
        // mBufVbo[mBufVboLen++] = color->g / 255.0f;
        // mBufVbo[mBufVboLen++] = color->b / 255.0f;
        // mBufVbo[mBufVboLen++] = color->a / 255.0f;
    }
#ifdef __3DS__
    GDX_TRI2_LAP(Pack);
    } // [triloop] legacy variable-stride append (packed batches wrote above)
#endif

    if (!is_rect) {
        mGeometryDiagnostics.trianglesEmitted++;
    }
    if (++mBufVboNumTris == MAX_TRI_BUFFER) {
        // if (++mBufVbo_num_tris == 1) {
        Flush();
    }
    GDX_TRI2_LAP(Tail);
}

void Interpreter::GfxSpGeometryMode(uint32_t clear, uint32_t set) {
    mRsp->geometry_mode &= ~clear;
    mRsp->geometry_mode |= set;
}

void Interpreter::GfxSpExtraGeometryMode(uint32_t clear, uint32_t set) {
    mRsp->extra_geometry_mode &= ~clear;
    mRsp->extra_geometry_mode |= set;
}

// Expand an L3DEX2 G_LINE3D into a screen-space quad: Fast3D has no line primitive, so the port's
// gfx bridge rewrites Course Edit's line commands into OTR_G_LINE3D_GDX and this draws each as two
// triangles of constant pixel width from the two already-transformed vertices.
void Interpreter::GfxSpLine3DGdx(uint8_t vtx1Idx, uint8_t vtx2Idx, uint8_t halfWidth) {
    const struct LoadedVertex a = mRsp->loaded_vertices[vtx1Idx];
    const struct LoadedVertex b = mRsp->loaded_vertices[vtx2Idx];

    /* Whether the handler fires in Course Edit at all, and whether the w <= 0 early-out is what
       suppresses the spline lines. */
    {
        static int sGdxLineDbg = 0;
        if (sGdxLineDbg < 32) {
            sGdxLineDbg++;
            gdx_dbg_logf("[GDX-DBG line] v0=%u v1=%u wd=%u aw=%.3f bw=%.3f %s\n",
                         vtx1Idx, vtx2Idx, halfWidth, a.w, b.w,
                         (a.w <= 0.0f || b.w <= 0.0f) ? "EARLYOUT(w<=0)" : "draw");
        }
    }

    // Both endpoints must be in front of the camera; near-plane clipping of partially
    // visible lines is not worth the complexity for the editor's on-canvas geometry.
    if (a.w <= 0.0f || b.w <= 0.0f) {
        return;
    }

    const float ax = a.x / a.w, ay = a.y / a.w;
    const float bx = b.x / b.w, by = b.y / b.w;

    // Direction in pixel space so the visual width is uniform regardless of line angle
    // and window aspect.
    const float pixelHalfW = (float)mCurDimensions.width * 0.5f;
    const float pixelHalfH = (float)mCurDimensions.height * 0.5f;
    const float dx = (bx - ax) * pixelHalfW;
    const float dy = (by - ay) * pixelHalfH;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) {
        return;
    }
    // Hardware line width: wd is in half-pixel units on top of a 1.5px base, and the
    // quad extends half that width to each side. Scale from the 320x240 native raster
    // to the current output so lines keep their authored proportion.
    const float nativeWidthPx = 1.5f + (float)halfWidth * 0.5f;
    const float outputScale = (float)mCurDimensions.height / 240.0f;
    const float halfPx = nativeWidthPx * 0.5f * (outputScale > 0.0f ? outputScale : 1.0f);
    const float perpX = (-dy / len) * halfPx / pixelHalfW; // back to NDC units
    const float perpY = (dx / len) * halfPx / pixelHalfH;

    struct LoadedVertex* quad = &mRsp->loaded_vertices[MAX_VERTICES + 0];
    quad[0] = a;
    quad[1] = a;
    quad[2] = b;
    quad[3] = b;
    quad[0].x = (ax + perpX) * a.w;
    quad[0].y = (ay + perpY) * a.w;
    quad[1].x = (ax - perpX) * a.w;
    quad[1].y = (ay - perpY) * a.w;
    quad[2].x = (bx + perpX) * b.w;
    quad[2].y = (by + perpY) * b.w;
    quad[3].x = (bx - perpX) * b.w;
    quad[3].y = (by - perpY) * b.w;
    quad[0].clip_rej = 0;
    quad[1].clip_rej = 0;
    quad[2].clip_rej = 0;
    quad[3].clip_rej = 0;
    // [traffic] x/y were rewritten above, so the copied clip_out masks are stale; the quads go
    // through the non-rect GfxSpTri1 path whose clip pre-scan now reads clip_out.
    quad[0].clip_out = GdxComputeClipOutMask(quad[0]);
    quad[1].clip_out = GdxComputeClipOutMask(quad[1]);
    quad[2].clip_out = GdxComputeClipOutMask(quad[2]);
    quad[3].clip_out = GdxComputeClipOutMask(quad[3]);

    // Hardware lines never face-cull; suppress culling for the quad only.
    const uint32_t savedGeometryMode = mRsp->geometry_mode;
    mRsp->geometry_mode &= ~get_attr(CULL_BOTH);
    GfxSpTri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 2, false);
    GfxSpTri1(MAX_VERTICES + 1, MAX_VERTICES + 3, MAX_VERTICES + 2, false);
    mRsp->geometry_mode = savedGeometryMode;
}

void Interpreter::AdjustVIewportOrScissor(XYWidthHeight* area) {
    if (!mFbActive) {
        // Adjust the y origin based on the y-inversion for the active framebuffer
        GfxClipParameters clipParameters = mRapi->GetClipParameters();
        if (clipParameters.invertY) {
            area->y -= area->height;
        } else {
            area->y = mNativeDimensions.height - area->y;
        }

        area->width *= RATIO_X(mActiveFrameBuffer, mCurDimensions);
        area->height *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);
        area->x *= RATIO_X(mActiveFrameBuffer, mCurDimensions);
        area->y *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);

        if (!mRendersToFb || (mMsaaLevel > 1 && mCurDimensions.width == mGameWindowViewport.width &&
                              mCurDimensions.height == mGameWindowViewport.height)) {
            area->x += mGameWindowViewport.x;
            area->y += mGfxCurrentWindowDimensions.height - (mGameWindowViewport.y + mGameWindowViewport.height);
        }
    } else {
        area->y = mActiveFrameBuffer->second.orig_height - area->y;

        if (mActiveFrameBuffer->second.resize) {
            area->width *= RATIO_X(mActiveFrameBuffer, mCurDimensions);
            area->height *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);
            area->x *= RATIO_X(mActiveFrameBuffer, mCurDimensions);
            area->y *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);
        }
    }
}

void Interpreter::CalcAndSetViewport(const F3DVp_t* viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    mRdp->viewport.x = x;
    mRdp->viewport.y = y;
    mRdp->viewport.width = width;
    mRdp->viewport.height = height;
    mRsp->viewport_z_scale = viewport->vscale[2];
    mRsp->viewport_z_trans = viewport->vtrans[2];

    AdjustVIewportOrScissor(&mRdp->viewport);

    mRdp->viewport_or_scissor_changed = true;
}

void Interpreter::GfxSpMovememF3dex2(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case F3DEX2_G_MV_VIEWPORT:
            CalcAndSetViewport((const F3DVp_t*)data);
            break;
        case F3DEX2_G_MV_LIGHT: {
            // F3DFLX2.Rej repurposes the LOOKATY slot as its reflection-alpha light, but only
            // after the game DMAs the alpha LUT into DMEM via G_DMA_IO. Menu tasks run the FLX
            // variant too and upload a genuine LookAt Y there; without the LUT gate that upload
            // was consumed and lookat[1] never updated, breaking texture-gen reflections.
            if (mF3dex2Variant == F3dex2Variant::FZeroFlxReject && offset == 24 && data != nullptr &&
                mRsp->dma_io_loaded) {
                // The alpha light is host-written GfxPool data (racer.c),
                // so its s16 direction fields are native-endian, not the
                // big-endian layout ROM data would have.
                const uint8_t* bytes = static_cast<const uint8_t*>(data);
                int16_t dir[3];
                memcpy(dir, bytes + 8, sizeof(dir));
                mRsp->f3dflx_alpha_light[0] = dir[0] / 256.0f;
                mRsp->f3dflx_alpha_light[1] = dir[1] / 256.0f;
                mRsp->f3dflx_alpha_light[2] = dir[2] / 256.0f;
                NormalizeVector(mRsp->f3dflx_alpha_light);
                mRsp->f3dflx_alpha_light_valid = true;
                break;
            }
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(mRsp->current_lights + lightidx, data, sizeof(F3DLight));
            } else if (lightidx < 0) {
                memcpy(mRsp->lookat + offset / 24, data, sizeof(F3DLight_t)); // TODO Light?
            }
            break;
        }
    }
}

void Interpreter::GfxSpDmaIo(bool write, uint16_t dmem, void* data, size_t size) {
    if (data == nullptr || dmem >= sizeof(mRsp->dmem)) {
        return;
    }
    const size_t copySize = std::min(size, sizeof(mRsp->dmem) - static_cast<size_t>(dmem));
    if (write) {
        memcpy(data, mRsp->dmem + dmem, copySize);
    } else {
        memcpy(mRsp->dmem + dmem, data, copySize);
        mRsp->dma_io_dmem = dmem;
        mRsp->dma_io_loaded = true;
        mGeometryDiagnostics.dmaIoLoads++;
    }
}

void Interpreter::GfxSpMovememF3d(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case F3DEX_G_MV_VIEWPORT:
            CalcAndSetViewport((const F3DVp_t*)data);
            break;
        case F3DEX_G_MV_LOOKATY:
        case F3DEX_G_MV_LOOKATX:
            memcpy(mRsp->lookat + (index - F3DEX_G_MV_LOOKATY) / 2, data, sizeof(F3DLight_t));
            break;
        case F3DEX_G_MV_L0:
        case F3DEX_G_MV_L1:
        case F3DEX_G_MV_L2:
        case F3DEX_G_MV_L3:
        case F3DEX_G_MV_L4:
        case F3DEX_G_MV_L5:
        case F3DEX_G_MV_L6:
        case F3DEX_G_MV_L7:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(mRsp->current_lights + (index - F3DEX_G_MV_L0) / 2, data, sizeof(F3DLight_t));
            break;
    }
}

void Interpreter::GfxSpMovewordF3dex2(uint8_t index, uint16_t offset, uintptr_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
            mRsp->current_num_lights = data / 24 + 1; // add ambient light
            mRsp->lights_changed = true;
            break;
        case G_MW_FOG:
#ifdef __3DS__
            // The 3DS backend applies fog per DRAW through a LUT built from ONE fog line
            // (gdx3ds_fog_note_params), but a G_MW_FOG moveword alone never flushed pending
            // tris (per-vertex baking did not need it). F-Zero X's race DLs retune
            // gSPFogPosition several times per frame with no intervening state change, so
            // tris baked under the previous line shared a batch with tris baked under the
            // new one — and the whole batch bound the LAST line (track chunks inheriting
            // the backdrop's shallower curve: the residual "invisible road"). Split the
            // batch when the line actually changes. Desktop interpolates the baked
            // per-vertex factors and needs no split.
            if (mRsp->fog_mul != (int16_t)(data >> 16) || mRsp->fog_offset != (int16_t)data) {
                Flush();
            }
#endif
            mRsp->fog_mul = (int16_t)(data >> 16);
            mRsp->fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT: {
            int segNumber = offset / 4;
            mSegmentPointers[segNumber] = data;
        } break;
        case G_MW_SEGMENT_INTERP: {
            int segNumber = offset % 16;
            int segIndex = offset / 16;

            if (segIndex == mInterpolationIndex)
                mSegmentPointers[segNumber] = data;
        } break;
    }
}

void Interpreter::GfxSpMovewordF3d(uint8_t index, uint16_t offset, uintptr_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
            // Ambient light is included
            // The 31st bit is a flag that lights should be recalculated
            mRsp->current_num_lights = (data - 0x80000000U) / 32;
            mRsp->lights_changed = true;
            break;
        case G_MW_FOG:
#ifdef __3DS__
            // Same per-draw fog-LUT batch split as the F3DEX2 handler above.
            if (mRsp->fog_mul != (int16_t)(data >> 16) || mRsp->fog_offset != (int16_t)data) {
                Flush();
            }
#endif
            mRsp->fog_mul = (int16_t)(data >> 16);
            mRsp->fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT: {
            int segNumber = offset / 4;
            mSegmentPointers[segNumber] = data;
        } break;
        case G_MW_SEGMENT_INTERP: {
            int segNumber = offset % 16;
            int segIndex = offset / 16;

            if (segIndex == mInterpolationIndex)
                mSegmentPointers[segNumber] = data;
        } break;
    }
}

void Interpreter::GfxSpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    mRsp->texture_scaling_factor.s = sc;
    mRsp->texture_scaling_factor.t = tc;
    if (mRdp->first_tile_index != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }

    mRdp->first_tile_index = tile;
}

void Interpreter::GfxDpSetScissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    // F-Zero X frames every screen inside the CRT-overscan-safe area via two scissors hardcoded at
    // ~50 call sites ({12,16,308,224} and {12,8,308,232}); intercepting them here is the one
    // chokepoint that also covers static DLs. Exact match only, so split-screen boxes and menu
    // reveal wipes never widen.
    if (mRemoveBordersCache && ulx == (12 << 2) && lrx == (308 << 2) &&
        ((uly == (16 << 2) && lry == (224 << 2)) || (uly == (8 << 2) && lry == (232 << 2)))) {
        ulx = 0;
        uly = 0;
        lrx = 320 << 2;
        lry = 240 << 2;
    }
    float x = ulx / 4.0f;
    float y = lry / 4.0f;
    float width = (lrx - ulx) / 4.0f;
    float height = (lry - uly) / 4.0f;

    mRdp->scissor.x = x;
    mRdp->scissor.y = y;
    mRdp->scissor.width = width;
    mRdp->scissor.height = height;

    AdjustVIewportOrScissor(&mRdp->scissor);

    mRdp->viewport_or_scissor_changed = true;
}

void Interpreter::GfxDpSetTextureImage(uint32_t format, uint32_t size, uint32_t width, const char* texPath,
                                       uint32_t texFlags, RawTexMetadata rawTexMetdata, const void* addr) {
    // fprintf(stderr, "GfxDpSetTextureImage: %s (width=%d; size=0x%X)\n",
    //         rawTexMetdata.resource ? rawTexMetdata.resource->GetInitData()->Path.c_str() : nullptr, width, size);
    mRdp->texture_to_load.addr = (const uint8_t*)addr;
    mRdp->texture_to_load.siz = size;
    mRdp->texture_to_load.width = width;
    mRdp->texture_to_load.tex_flags = texFlags;
    mRdp->texture_to_load.raw_tex_metadata = rawTexMetdata;
}

void Interpreter::GfxDpSetTile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                               uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,
                               uint32_t shifts) {
    // OTRTODO:
    // SUPPORT_CHECK(tmem == 0 || tmem == 256);

    // Trace tile-descriptor writes so stale-state bugs (a draw inheriting another
    // object's SETTILE) can be correlated against draw markers in the same file.
    static const bool diagSetTile = std::getenv("GDX_DIAG_SETTILE") != nullptr;
    if (diagSetTile) {
        extern int gGdxTraceSeq;
        extern void GdxTileTracePush(const char* line);
        char buf[160];
        snprintf(buf, sizeof(buf), "S %06d tile=%u fmt=%u siz=%u line=%u tmem=0x%X cms=%u masks=%u cmt=%u maskt=%u",
                 ++gGdxTraceSeq, tile, fmt, siz, line, tmem, cms, masks, cmt, maskt);
        GdxTileTracePush(buf);
        // The course setup DL is the only source of masks==7 tile writes; log those
        // executions directly (bypassing the ring) to prove whether they ever run.
        if (masks == 7 && gGdxRaceActive != 0) {
            static int sWideTileCount = 0;
            if (sWideTileCount < 64) {
                ++sWideTileCount;
                FILE* wf = fopen("settile-trace.txt", "a");
                if (wf != nullptr) {
                    fprintf(wf, "W %06d %s\n", gGdxTraceSeq, buf);
                    fclose(wf);
                }
            }
        }
    }

    if (cms == G_TX_WRAP && masks == G_TX_NOMASK) {
        cms = G_TX_CLAMP;
    }
    if (cmt == G_TX_WRAP && maskt == G_TX_NOMASK) {
        cmt = G_TX_CLAMP;
    }

    // [crowd2] Value-gated re-assertion skip: F-Zero X race DLs re-issue bit-identical
    // SETTILEs per machine part (~513/frame in traffic), each of which armed
    // textures_changed and dirtied the draw memos, forcing the next TRI into the full
    // Flush+ImportTexture+extent re-derivation path. When every field this handler would
    // write (compared post-canonicalization) already holds the incoming value, executing
    // the write is a no-op — so the arm and the dirty are skipped too. Soundness leans on
    // the mutation-site invariant: every OTHER way the derived draw state can change
    // (LOADBLOCK/LOADTILE stores, LOADTLUT palettes, cache evictions nulling bindings,
    // image-rect in-place descriptor rewrites) arms textures_changed at its own site.
    {
        const auto& t = mRdp->texture_tile[tile];
        if (t.palette == palette && t.fmt == fmt && t.siz == siz && t.cms == cms && t.cmt == cmt &&
            t.masks == masks && t.maskt == maskt && t.shifts == shifts && t.shiftt == shiftt &&
            t.line_size_bytes == line * 8 && t.tmem == tmem &&
            t.tmem_index == static_cast<uint16_t>(tmem)) {
            return;
        }
    }

    mRdp->texture_tile[tile].palette = palette; // palette should set upper 4 bits of color index in 4b mode
    mRdp->texture_tile[tile].fmt = fmt;
    mRdp->texture_tile[tile].siz = siz;
    mRdp->texture_tile[tile].cms = cms;
    mRdp->texture_tile[tile].cmt = cmt;
    mRdp->texture_tile[tile].masks = masks;
    mRdp->texture_tile[tile].maskt = maskt;
    mRdp->texture_tile[tile].shifts = shifts;
    mRdp->texture_tile[tile].shiftt = shiftt;
    mRdp->texture_tile[tile].line_size_bytes = line * 8;

    mRdp->texture_tile[tile].tmem = tmem;
    mRdp->texture_tile[tile].tmem_index = static_cast<uint16_t>(tmem);

    // [crowd2] SETTILE is whitelisted in the flat-dispatch dirty table now (its precomputed
    // contribution is zero); a real descriptor change re-creates the legacy contribution here.
    sGfxS7DirtyMask |= static_cast<uint8_t>(kGfxS7DirtyTri | kGfxS7DirtyRect | kGfxS7DirtyRectViewport);
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
}

void Interpreter::GfxDpSetTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    // [crowd2] Same value-gated skip as GfxDpSetTile: a bit-identical re-assertion leaves
    // no observable state behind, so it must not arm imports or dirty the draw memos.
    {
        const auto& t = mRdp->texture_tile[tile];
        if (t.uls == uls && t.ult == ult && t.lrs == lrs && t.lrt == lrt) {
            return;
        }
    }
    mRdp->texture_tile[tile].uls = uls;
    mRdp->texture_tile[tile].ult = ult;
    mRdp->texture_tile[tile].lrs = lrs;
    mRdp->texture_tile[tile].lrt = lrt;
    sGfxS7DirtyMask |= static_cast<uint8_t>(kGfxS7DirtyTri | kGfxS7DirtyRect | kGfxS7DirtyRectViewport);
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
}

void Interpreter::GfxDpLoadTlut(uint8_t tile, uint32_t high_index) {
    SUPPORT_CHECK(mRdp->texture_to_load.siz == G_IM_SIZ_16b);

    uint16_t tmem = mRdp->texture_tile[tile].tmem;
    const uint8_t* src = mRdp->texture_to_load.addr;
    uint32_t entryCount = high_index + 1;
    uint32_t byteCount = entryCount * 2;

    // Record every address ever bound as a TLUT source: the O(1) gate that keeps
    // TextureCacheDeletePalette from scanning the whole cache on an ordinary texture refresh.
    if (src != nullptr) {
        mSeenPaletteAddrs.insert(src);
    }

    if (tmem >= 256) {
        // N64 TMEM palette area starts at tmem word 256. Each CI4 palette = 16 entries = 16 tmem words.
        uint32_t paletteByteOffset = (tmem - 256) * 2;

        if (high_index == 255 && paletteByteOffset == 0) {
            // CI8: full 256-entry palette spanning both halves
            memcpy(mRdp->palette_staging[0], src, 256);
            memcpy(mRdp->palette_staging[1], src + 256, 256);
            mRdp->palettes[0] = mRdp->palette_staging[0];
            mRdp->palettes[1] = mRdp->palette_staging[1];
            mRdp->palette_dram_addr[0] = src;
            mRdp->palette_dram_addr[1] = src + 256;
        } else if (paletteByteOffset < 256) {
            // Palettes 0-7 range
            uint32_t copyLen = (paletteByteOffset + byteCount <= 256) ? byteCount : (256 - paletteByteOffset);
            memcpy(mRdp->palette_staging[0] + paletteByteOffset, src, copyLen);
            mRdp->palettes[0] = mRdp->palette_staging[0];
            mRdp->palette_dram_addr[0] = src;
        } else {
            // Palettes 8-15 range
            uint32_t offset = paletteByteOffset - 256;
            uint32_t copyLen = (offset + byteCount <= 256) ? byteCount : (256 - offset);
            memcpy(mRdp->palette_staging[1] + offset, src, copyLen);
            mRdp->palettes[1] = mRdp->palette_staging[1];
            mRdp->palette_dram_addr[1] = src;
        }
    } else {
        // tmem < 256: non-standard location, fall back to direct pointer
        mRdp->palettes[1] = src;
        mRdp->palette_dram_addr[1] = src;
    }

    // [crowd2] A palette (re)load changes what a CI import decodes to, but LOADTLUT never
    // armed textures_changed — the unconditional SETTILE re-arm on the way to the next draw
    // was the rescue. That re-arm is now value-gated, so the palette mutation site must arm.
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
}

// [tmem2] Live-span list maintenance (see RDP::tmem_span_list). Both store paths keep it
// consistent so the killswitch can flip between frames.
static constexpr uint16_t kTmem2ListInvalid = 0xFFFFu;
static constexpr uint32_t kTmem2ListCap = 64;
static inline void GdxTmem2ListRemove(RDP& rdp, uint32_t base) {
    if (rdp.tmem_span_list_n == kTmem2ListInvalid) {
        return;
    }
    for (uint32_t i = 0; i < rdp.tmem_span_list_n; ++i) {
        if (rdp.tmem_span_list[i] == base) {
            rdp.tmem_span_list[i] = rdp.tmem_span_list[--rdp.tmem_span_list_n];
            return;
        }
    }
}
static inline void GdxTmem2ListInsert(RDP& rdp, uint32_t base) {
    if (rdp.tmem_span_list_n == kTmem2ListInvalid) {
        return;
    }
    for (uint32_t i = 0; i < rdp.tmem_span_list_n; ++i) {
        if (rdp.tmem_span_list[i] == base) {
            return;
        }
    }
    if (rdp.tmem_span_list_n >= kTmem2ListCap) {
        rdp.tmem_span_list_n = kTmem2ListInvalid;
        return;
    }
    rdp.tmem_span_list[rdp.tmem_span_list_n++] = static_cast<uint16_t>(base);
}
static bool GdxTmem2ListRebuild(RDP& rdp) {
    uint32_t n = 0;
    for (uint32_t w = 0; w < 512u; ++w) {
        if (rdp.tmem_span_base[w] == static_cast<uint16_t>(w + 1u)) {
            if (n >= kTmem2ListCap) {
                rdp.tmem_span_list_n = kTmem2ListInvalid;
                return false;
            }
            rdp.tmem_span_list[n++] = static_cast<uint16_t>(w);
        }
    }
    rdp.tmem_span_list_n = static_cast<uint16_t>(n);
    return true;
}

// [tmem2] tmemfast teardown (see StoreLoadedTexture). Finds the spans overlapping
// [start, end) through the live-span list — never through the per-word map — checks them
// all first (a bail-out leaves the map untouched for the legacy walk), then mutates.
// Accepted shapes: any set of spans with no fresh interior view and a base record that
// still describes the span. Returns false for anything else.
static bool GdxTmem2FastTeardown(RDP& rdp, uint32_t start, uint32_t end, uint32_t wordCount, bool& sameRange) {
    if (rdp.tmem_span_list_n == kTmem2ListInvalid && !GdxTmem2ListRebuild(rdp)) {
        gdx_tmem2_stat[kTmem2StMissList]++;
        return false;
    }
    uint32_t overlapping = 0;
    bool same = false;
    for (uint32_t i = 0; i < rdp.tmem_span_list_n;) {
        const uint32_t base = rdp.tmem_span_list[i];
        if (rdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u)) {
            rdp.tmem_span_list[i] = rdp.tmem_span_list[--rdp.tmem_span_list_n]; // dead entry
            continue;
        }
        const uint32_t words = rdp.tmem_span_words[base];
        const uint32_t spanEnd = base + words;
        ++i;
        if (words == 0 || spanEnd <= start || base >= end) {
            continue;
        }
        if (rdp.tmem_span_fresh[base] != 0) {
            gdx_tmem2_stat[kTmem2StMissInterior]++;
            return false; // interior views to release: legacy walk
        }
        if (rdp.tmem_slot_stale[base] == 0) {
            // A fresh base must still describe exactly this span. A detached base (an
            // in-place mutator zeroed its word count) is the one shape the legacy walk
            // leaves inconsistent after a partial overwrite, so it takes the legacy walk.
            const LoadedTexture& baseEntry = rdp.loaded_texture[base];
            if (baseEntry.tmem_start != base || baseEntry.tmem_word_count != words) {
                gdx_tmem2_stat[kTmem2StMissDivergent]++;
                return false;
            }
        }
        overlapping++;
        same = base == start && words == wordCount;
    }
    // Mutate: release each overlapping span's fresh base (the refs the legacy walk
    // released), drop its base-indexed records, unmap the words it owned OUTSIDE the new
    // range (the record step re-maps the inside ones), and delist it. Interior words are
    // all stale by the check above, exactly as after the legacy walk.
    for (uint32_t i = 0; i < rdp.tmem_span_list_n;) {
        const uint32_t base = rdp.tmem_span_list[i];
        const uint32_t words = rdp.tmem_span_words[base];
        const uint32_t spanEnd = base + words;
        if (spanEnd <= start || base >= end) {
            ++i;
            continue;
        }
        if (rdp.tmem_slot_stale[base] == 0) {
            rdp.loaded_texture[base] = {};
        }
        rdp.tmem_span_words[base] = 0;
        rdp.tmem_span_load_kind[base] = 0;
        rdp.tmem_span_fresh[base] = 0;
        if (base < start) {
            std::fill_n(&rdp.tmem_span_base[base], start - base, static_cast<uint16_t>(0));
        }
        if (spanEnd > end) {
            std::fill_n(&rdp.tmem_span_base[end], spanEnd - end, static_cast<uint16_t>(0));
        }
        rdp.tmem_span_list[i] = rdp.tmem_span_list[--rdp.tmem_span_list_n];
    }
    sameRange = overlapping == 1 && same;
    return true;
}

void Interpreter::StoreLoadedTexture(uint16_t tmemStart, const LoadedTexture& texture) {
    constexpr uint32_t kTmemWordBytes = 8;
    constexpr uint32_t kTmemWordCount = 512;
    if (tmemStart >= kTmemWordCount) {
        return;
    }

    // Race-gate FIRST (a plain int test), then latch the env probe on the first race-active
    // call. The bridge exports GDX_DIAG_SETTIMG via _putenv at the first real GFX task, which
    // always precedes race activation, so a race-time latch sees the final value — while the
    // previous per-call live getenv was an environ scan on EVERY TMEM store (~82/frame in
    // menus) even with the gate off.
    if (gGdxRaceActive != 0) { // race-gated: boot stores ate the whole budget otherwise
        static const bool sTmemTrace = std::getenv("GDX_DIAG_SETTIMG") != nullptr;
        if (sTmemTrace) {
            static int sTmemStoreLogs = 0;
            if (sTmemStoreLogs < 400) {
                ++sTmemStoreLogs;
                SPDLOG_ERROR("[tmem] store start={} sizeB={} origB={} addr={}", tmemStart,
                             texture.size_bytes, texture.orig_size_bytes, fmt::ptr(texture.addr));
            }
        }
    }

#ifdef __3DS__
    // SHIP-LIVERY-2: every TMEM store, with the source's resource identity when one
    // exists. Early-returns port-side while the [livery] window is disarmed.
    gdx3ds_livery_note_load((unsigned)tmemStart,
                            texture.raw_tex_metadata.resource != nullptr
                                ? texture.raw_tex_metadata.resource->GetInitData()->Path.c_str()
                                : nullptr,
                            texture.addr, texture.size_bytes);
#endif

    const bool tmem2On = GdxTmem2ProfOn(); // [tmem2]
    const uint64_t tmem2TA = GdxTmem2Now(tmem2On);
    const uint32_t logicalSize = texture.orig_size_bytes != 0 ? texture.orig_size_bytes : texture.size_bytes;
    const uint32_t requestedWords = std::max<uint32_t>(1u, (logicalSize + kTmemWordBytes - 1u) / kTmemWordBytes);
    const uint32_t wordCount = std::min(requestedWords, kTmemWordCount - tmemStart);
    const uint32_t newEnd = tmemStart + wordCount;
    RDP& rdp = *mRdp;

    // A TMEM upload replaces bytes, not a named texture object. Discard any
    // older metadata range touched by this upload so a later render tile can
    // never select stale DRAM data for overwritten TMEM.
    //
    // LOADBLOCK-OPT: the old implementation walked all 512 per-word LoadedTexture entries
    // and cleared every one whose own recorded [tmem_start, +tmem_word_count) range
    // overlapped the new range. The span map reaches the same set of words in O(touched
    // words): every such entry belongs to a span with at least one word inside the new
    // range, and only slots that were actually materialized still hold payloads (and
    // resource refs) to release — exactly the refs the old eager clears released. The
    // per-word overlap re-check below keeps one legacy subtlety intact: an entry whose own
    // recorded word count is 0 (the port's VI-fallback slot detaches itself that way) was
    // never cleared by the old walk, and still is not.
    // [tmem2] tmemfast: O(1) span replacement. Measured in a 30-machine crowd (emulator,
    // [tmem2] census): every load replaces exactly ONE live span (span == calls) whose only
    // fresh slot is its base (rel == calls), yet the legacy walk below visits every word of
    // that span (walk == words, ~100 per load) to zero three per-word arrays that the record
    // step then rewrites for the very same words — ~50 % of the F3 handler. When every span
    // touching [tmemStart, newEnd) lies entirely inside it and has no fresh interior slot,
    // the teardown is: release each such span's fresh base (exactly the refs the walk
    // released), drop its base-indexed records, unmap only the words outside the new
    // range, done — found through a live-span list, so no TMEM word is visited. If the old
    // span IS this range (same base, same word count) the per-word span_base fill and
    // stale stamp are no-ops as well. Fresh interior views or a detached/divergent base
    // fall through to the legacy walk unchanged. Off = the legacy walk, always.
    bool tmem2SameRange = false;
    bool tmem2FastDone = false;
    if (gdx_tmem2_fast_enabled != 0) {
        tmem2FastDone = GdxTmem2FastTeardown(rdp, tmemStart, newEnd, wordCount, tmem2SameRange);
        gdx_tmem2_stat[tmem2FastDone ? kTmem2StFastHit : kTmem2StFastMiss]++;
    }

    const auto slotRange = [&rdp](uint32_t word, uint32_t& start, uint32_t& end) {
        if (rdp.tmem_slot_stale[word] == 0) {
            // Fresh slot: its own recorded range is authoritative (old-code semantics,
            // including in-place mutations that detached the entry).
            start = rdp.loaded_texture[word].tmem_start;
            end = start + rdp.loaded_texture[word].tmem_word_count;
        } else if (rdp.tmem_span_base[word] != 0) {
            // Stale slot: the range materialization WOULD record is the span's.
            start = rdp.tmem_span_base[word] - 1u;
            end = start + rdp.tmem_span_words[start];
        } else {
            start = end = 0;
        }
    };
    uint32_t w = tmem2FastDone ? newEnd : tmemStart; // [tmem2] fast teardown already ran
    while (w < newEnd) {
        uint32_t spanStart;
        uint32_t spanEnd;
        slotRange(w, spanStart, spanEnd);
        if (spanEnd <= spanStart) {
            ++w;
            continue;
        }
        gdx_tmem2_stat[kTmem2StWalkSpans]++; // [tmem2]
        if (spanStart < kTmemWordCount) {
            rdp.tmem_span_fresh[spanStart] = 0; // [tmem2] the span (or claimed base) is going away
            GdxTmem2ListRemove(rdp, spanStart);
        }
        for (uint32_t k = spanStart; k < spanEnd && k < kTmemWordCount; ++k) {
            gdx_tmem2_stat[kTmem2StWalkIters]++; // [tmem2]
            // A stale member's logical range IS the span's (captured above — the map entries
            // are being torn down as this loop runs); only a fresh slot can carry a diverging
            // record of its own (the detached zero-count case).
            uint32_t kStart = spanStart;
            uint32_t kEnd = spanEnd;
            if (rdp.tmem_slot_stale[k] == 0) {
                kStart = rdp.loaded_texture[k].tmem_start;
                kEnd = kStart + rdp.loaded_texture[k].tmem_word_count;
            }
            if (kStart < newEnd && tmemStart < kEnd) {
                if (rdp.tmem_slot_stale[k] == 0) {
                    // Fresh: release the materialized view (and its resource ref) now, exactly
                    // where the old eager walk released it; the slot stays fresh because a
                    // physical {} equals the logical empty state.
                    rdp.loaded_texture[k] = {};
                    gdx_tmem2_stat[kTmem2StWalkRelease]++; // [tmem2]
                } // else: stale slots are ref-free by invariant; span_base = 0 below makes the
                  // next read materialize the {}.
                rdp.tmem_span_base[k] = 0;
                rdp.tmem_span_words[k] = 0;
                rdp.tmem_span_load_kind[k] = 0;
            }
        }
        w = spanEnd > w ? spanEnd : w + 1;
    }
    const uint64_t tmem2TB = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhWalk, tmem2TA, tmem2TB);
    gdx_tmem2_stat[kTmem2StWords] += wordCount;

    // Record ONE span for the new load. Render tiles are legal at offsets inside a
    // LOADBLOCK/LOADTILE range, not only at its base, so the old code materialized a
    // derived view at every covered word here; those views now materialize lazily in
    // RDP::MaterializeTmemSlot (same math, verbatim) on first read.
    if (!tmem2SameRange) { // [tmem2] same-range replacement: every word already maps here
        std::fill_n(&rdp.tmem_span_base[tmemStart], wordCount, static_cast<uint16_t>(tmemStart + 1u));
        // (and, with no fresh interior view — the fast path's precondition — every interior
        // word is already stale, so the stale stamp below is a no-op there too)
        memset(&rdp.tmem_slot_stale[tmemStart], 1, wordCount);
    }
    GdxTmem2ListInsert(rdp, tmemStart);
    rdp.tmem_span_words[tmemStart] = static_cast<uint16_t>(wordCount);
    rdp.tmem_span_load_kind[tmemStart] = 0; // caller re-arms after a completed TMEM mirror
    rdp.tmem_span_fresh[tmemStart] = 0;     // [tmem2] fresh span: no interior views yet

    if (tmem2FastDone) {
        // [tmem2] One copy straight into the slot (the legacy copy + move pair below lands
        // the same bytes; the old base payload is released by the assignment either way).
        LoadedTexture& baseSlot = rdp.loaded_texture[tmemStart];
        baseSlot = texture;
        baseSlot.tmem_start = tmemStart;
        baseSlot.tmem_word_count = static_cast<uint16_t>(wordCount);
    } else {
        LoadedTexture baseEntry = texture;
        baseEntry.tmem_start = tmemStart;
        baseEntry.tmem_word_count = static_cast<uint16_t>(wordCount);
        rdp.loaded_texture[tmemStart] = std::move(baseEntry);
    }
    rdp.tmem_slot_stale[tmemStart] = 0;
    GdxTmem2Acc(tmem2On, kTmem2PhRecord, tmem2TB, GdxTmem2Now(tmem2On)); // [tmem2]
}

// Lazy per-word view materialization: byte-identical to the loop StoreLoadedTexture ran
// eagerly at every covered word before LOADBLOCK-OPT (see the RDP struct comment).
void RDP::MaterializeTmemSlot(uint16_t tmemWord) {
    constexpr uint32_t kTmemWordBytes = 8;
    const uint16_t basePlus1 = tmem_span_base[tmemWord];
    if (basePlus1 == 0) {
        loaded_texture[tmemWord] = {};
        tmem_slot_stale[tmemWord] = 0;
        return;
    }
    const uint16_t base = static_cast<uint16_t>(basePlus1 - 1u);
    // The base slot doubles as the span record: it always holds the span's word-0 view,
    // whose payload fields equal the original StoreLoadedTexture argument.
    const LoadedTexture& texture = loaded_texture[base];
    const uint32_t word = static_cast<uint32_t>(tmemWord) - base;
    const uint32_t logicalSize = texture.orig_size_bytes != 0 ? texture.orig_size_bytes : texture.size_bytes;
    const uint32_t logicalByteOffset = word * kTmemWordBytes;
    const uint32_t hostByteOffset =
        logicalSize != 0
            ? static_cast<uint32_t>((static_cast<uint64_t>(logicalByteOffset) * texture.size_bytes) / logicalSize)
            : logicalByteOffset;
    LoadedTexture entry = texture; // tmem_start/tmem_word_count already carry the span's values
    if (entry.addr != nullptr) {
        entry.addr += hostByteOffset;
    }
    entry.size_bytes = texture.size_bytes > hostByteOffset ? texture.size_bytes - hostByteOffset : 0;
    entry.orig_size_bytes = texture.orig_size_bytes > logicalByteOffset ? texture.orig_size_bytes - logicalByteOffset : 0;
    if (word != 0) {
        // An interior TMEM address starts a new render view into this upload. Its row layout
        // comes from the render tile, not the load base; keeping the base stride collapses
        // these views into one-pixel-high stripes.
        entry.line_size_bytes = 0;
        entry.full_image_line_size_bytes = 0;
        tmem_span_fresh[base]++; // [tmem2] one more interior slot to release at teardown
    }
    loaded_texture[tmemWord] = std::move(entry);
    tmem_slot_stale[tmemWord] = 0;
}

// Write access for the in-place mutators (GfxDpImageRectangle, the port's VI present
// fallback): materialize EVERY word of the containing span first, so the edit lands on
// fully-consistent physical state — after this, no lazy derivation can run against the
// mutated record while the span lives, which is exactly the old always-materialized
// behaviour.
LoadedTexture& RDP::MutableLoadedTextureAt(uint16_t tmemWord) {
    const uint16_t basePlus1 = tmem_span_base[tmemWord];
    if (basePlus1 != 0) {
        const uint32_t base = basePlus1 - 1u;
        const uint32_t end = std::min<uint32_t>(base + tmem_span_words[base], 512u);
        for (uint32_t k = base; k < end; ++k) {
            if (tmem_slot_stale[k] != 0) {
                MaterializeTmemSlot(static_cast<uint16_t>(k));
            }
        }
    } else if (tmem_slot_stale[tmemWord] != 0) {
        MaterializeTmemSlot(tmemWord);
    }
    return loaded_texture[tmemWord];
}

// LOADBLOCK-OPT same-content skip support: after a loader has stored its span AND fully
// mirrored the transfer into emulated TMEM, record WHICH loader wrote it (the TMEM layout
// of a block copy and a row-strided tile copy differ for unaligned lines, so a skip must
// only match against the same loader kind). Also revoke skip eligibility from any OTHER
// span whose TMEM bytes this mirror overwrote beyond the new span's own word range: the
// old code simply clobbered the neighbour's TMEM mirror and a re-run of the neighbour's
// load would rewrite it, so a skip of that neighbour must not preserve the clobber.
// `tmemWriteEndByte` is the absolute end (exclusive) of the bytes actually written to
// mRdp->tmem, 0 when the mirror did not run.
static void GdxNoteTmemLoadKind(RDP& rdp, uint16_t base, uint8_t kind, uint32_t tmemWriteEndByte) {
    if (base >= 512u || rdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u)) {
        return; // StoreLoadedTexture rejected the store (out-of-range base)
    }
    rdp.tmem_span_load_kind[base] = kind;
    const uint32_t spanEnd = base + rdp.tmem_span_words[base];
    const uint32_t writeEndWord = std::min<uint32_t>((tmemWriteEndByte + 7u) / 8u, 512u);
    for (uint32_t w = spanEnd; w < writeEndWord; ++w) {
        if (rdp.tmem_span_base[w] != 0) {
            rdp.tmem_span_load_kind[rdp.tmem_span_base[w] - 1u] = 0;
        }
    }
}

// The skip itself is 3DS-only as a policy choice (the desktop path was never the measured
// bottleneck, and Windows' TmemSourceReadableLimit VirtualQuery bound can legitimately vary
// between identical calls); the constant keeps the code compiled everywhere.
#ifdef __3DS__
static constexpr bool kGdxTmemSameContentSkip = true;
#else
static constexpr bool kGdxTmemSameContentSkip = false;
#endif

// True when re-running a loader would reproduce the current span, slots and TMEM mirror
// bit-for-bit, so the whole store (invalidation walk, span rewrite, memcpy) can be
// skipped. Soundness argument, per condition:
//  - the span at `base` is live, its base slot fresh and un-detached (word_count matches):
//    live spans are disjoint and every TMEM-mirror overwrite that could desynchronize this
//    span's TMEM bytes either removed the span (overlap invalidation) or revoked its
//    load_kind stamp (GdxNoteTmemLoadKind overhang sweep), so span-live + kind-armed
//    implies the TMEM mirror still holds THIS load's bytes;
//  - the freshly derived payload equals the stored one field-for-field: the mirror's
//    copy extent is a pure function of (size_bytes, base) on this platform (readable
//    limit is SIZE_MAX outside _WIN32) and the tile row layout is a pure function of the
//    payload's line/full-line/orig sizes, so equal payloads imply an identical write;
//  - the source bytes cannot have changed: gated on a resource-backed SETTIMG
//    (raw_tex_metadata.resource != nullptr). The stored span holds a shared_ptr to the
//    Fast::Texture, so the resource object — and its ImageData the addr points into —
//    is pinned alive (no address recycling), and this port never mutates decoded o2r
//    payloads in place (no hot reload on the 3DS). Raw RDRAM/framebuffer sources have no
//    such guarantee and always take the full store path.
static bool GdxTmemSameContentLoad(RDP& rdp, uint16_t base, uint8_t kind, const LoadedTexture& fresh) {
    if (base >= 512u || fresh.raw_tex_metadata.resource == nullptr) {
        return false;
    }
    const uint32_t logicalSize = fresh.orig_size_bytes != 0 ? fresh.orig_size_bytes : fresh.size_bytes;
    const uint32_t wantWords =
        std::min<uint32_t>(std::max<uint32_t>(1u, (logicalSize + 7u) / 8u), 512u - base);
    if (rdp.tmem_span_base[base] != static_cast<uint16_t>(base + 1u) || rdp.tmem_slot_stale[base] != 0 ||
        rdp.tmem_span_load_kind[base] != kind || rdp.tmem_span_words[base] != wantWords) {
        return false;
    }
    const LoadedTexture& prev = rdp.loaded_texture[base];
    return prev.tmem_word_count == wantWords && prev.addr == fresh.addr &&
           prev.orig_size_bytes == fresh.orig_size_bytes && prev.size_bytes == fresh.size_bytes &&
           prev.full_image_line_size_bytes == fresh.full_image_line_size_bytes &&
           prev.line_size_bytes == fresh.line_size_bytes && prev.tex_flags == fresh.tex_flags &&
           prev.masked == fresh.masked && prev.blended == fresh.blended &&
           prev.raw_tex_metadata.resource.get() == fresh.raw_tex_metadata.resource.get() &&
           prev.raw_tex_metadata.width == fresh.raw_tex_metadata.width &&
           prev.raw_tex_metadata.height == fresh.raw_tex_metadata.height &&
           prev.raw_tex_metadata.h_byte_scale == fresh.raw_tex_metadata.h_byte_scale &&
           prev.raw_tex_metadata.v_pixel_scale == fresh.raw_tex_metadata.v_pixel_scale &&
           prev.raw_tex_metadata.type == fresh.raw_tex_metadata.type &&
           prev.raw_tex_metadata.path_hash == fresh.raw_tex_metadata.path_hash;
}

void Interpreter::GfxDpLoadBlock(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    const bool tmem2On = GdxTmem2ProfOn(); // [tmem2] phase timers (gputrace-gated)
    const uint64_t tmem2T0 = GdxTmem2Now(tmem2On);
    gdx_tmem2_stat[kTmem2StLbCalls]++;
    const auto byteCountForTexels = [](uint64_t texels, uint8_t siz) -> uint32_t {
        uint64_t bytes = 0;
        switch (siz) {
            case G_IM_SIZ_4b:
                bytes = (texels + 1u) / 2u;
                break;
            case G_IM_SIZ_8b:
                bytes = texels;
                break;
            case G_IM_SIZ_16b:
                bytes = texels * 2u;
                break;
            case G_IM_SIZ_32b:
                bytes = texels * 4u;
                break;
            default:
                break;
        }
        return static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX));
    };
    const uint32_t orig_size_bytes = byteCountForTexels(static_cast<uint64_t>(lrs) + 1u,
                                                        mRdp->texture_to_load.siz);
    uint32_t size_bytes = orig_size_bytes;
    if (mRdp->texture_to_load.raw_tex_metadata.h_byte_scale != 1 ||
        mRdp->texture_to_load.raw_tex_metadata.v_pixel_scale != 1) {
        size_bytes *= mRdp->texture_to_load.raw_tex_metadata.h_byte_scale;
        size_bytes *= mRdp->texture_to_load.raw_tex_metadata.v_pixel_scale;
    }
    const uint16_t tmemIndex = mRdp->texture_tile[tile].tmem_index;
    LoadedTexture loaded{};
    loaded.orig_size_bytes = orig_size_bytes;
    loaded.size_bytes = size_bytes;

    // G_LOADBLOCK's SETTIMG size is a transfer size, not necessarily the
    // texture's logical size. I4/I8 macros deliberately transfer as 16-bit.
    // When a render tile for the same TMEM slot already describes this image,
    // use its logical line layout for DRAM row stepping and texture import.
    uint32_t logical_line_bytes = 0;
    for (uint32_t renderTile = 0; renderTile < 8; ++renderTile) {
        const auto& candidateTile = mRdp->texture_tile[renderTile];
        if (renderTile == tile || candidateTile.tmem_index != tmemIndex ||
            candidateTile.line_size_bytes == 0 || mRdp->texture_to_load.width <= 1) {
            continue;
        }
        uint32_t candidateTmemLineBytes = candidateTile.line_size_bytes;
        if (candidateTile.siz == G_IM_SIZ_32b) {
            candidateTmemLineBytes *= 2u;
        }

        // LOADBLOCK's DXT encodes the number of 64-bit TMEM words per source row, which is
        // authoritative when the transfer size differs from the logical render size (I4 data
        // transferred as 16-bit words): the descriptor width can then describe only the visible
        // region while DXT still preserves the real source stride.
        if (dxt != 0 && mRdp->texture_to_load.siz != candidateTile.siz) {
            constexpr uint32_t kDxtOne = 1u << 11;
            const uint32_t dxtWordsPerLine = (kDxtOne + dxt - 1u) / dxt;
            const uint32_t dxtLineBytes = dxtWordsPerLine * 8u;
            if (dxtLineBytes == candidateTmemLineBytes &&
                dxtLineBytes <= size_bytes && size_bytes % dxtLineBytes == 0) {
                logical_line_bytes = dxtLineBytes;
                break;
            }
        }

        // SETTIMG may deliberately use a wider transfer format than the render tile (I4 through a
        // 16-bit LOADBLOCK). The tile's line field describes padded TMEM storage while DRAM rows
        // stay tightly packed, so derive the source stride from the image width and accept it only
        // when its 8-byte-aligned size matches the configured tile line.
        const uint32_t candidateSourceLineBytes =
            byteCountForTexels(mRdp->texture_to_load.width, candidateTile.siz);
        const uint32_t candidateAlignedTmemLineBytes =
            (candidateSourceLineBytes + 7u) & ~7u;
        if (candidateSourceLineBytes != 0 &&
            candidateTmemLineBytes == candidateAlignedTmemLineBytes &&
            candidateSourceLineBytes <= size_bytes &&
            size_bytes % candidateSourceLineBytes == 0) {
            logical_line_bytes = candidateSourceLineBytes;
            break;
        }
    }

    // Fall back to the SETTIMG transfer layout when no logical render tile is
    // available yet. Width 1 is the standard block-load sentinel and means the
    // entire transfer is one contiguous line until SETTILE supplies the layout.
    uint32_t source_line_bytes = logical_line_bytes != 0 ? logical_line_bytes : size_bytes;
    if (mRdp->texture_to_load.width > 1) {
        const uint32_t transferLineBytes =
            byteCountForTexels(mRdp->texture_to_load.width, mRdp->texture_to_load.siz);
        if (logical_line_bytes == 0 && transferLineBytes > 0 &&
            transferLineBytes <= size_bytes && size_bytes % transferLineBytes == 0) {
            source_line_bytes = transferLineBytes;
        }
    }
    loaded.line_size_bytes = source_line_bytes;
    loaded.full_image_line_size_bytes = source_line_bytes;
    // assert(size_bytes <= 4096 && "bug: too big texture");
    loaded.tex_flags = mRdp->texture_to_load.tex_flags;
    loaded.raw_tex_metadata = mRdp->texture_to_load.raw_tex_metadata;
    const size_t rowOffset = static_cast<size_t>(ult) * source_line_bytes;
    const size_t columnOffset = byteCountForTexels(uls, mRdp->texture_to_load.siz);
    loaded.addr = mRdp->texture_to_load.addr != nullptr
                      ? mRdp->texture_to_load.addr + rowOffset + columnOffset
                      : nullptr;
    // fprintf(stderr, "GfxDpLoadBlock: line_size = 0x%x; orig = 0x%x; bpp=%d; lrs=%d\n", size_bytes,
    // orig_size_bytes,
    //         mRdp->texture_to_load.siz, lrs);
    const uint64_t tmem2T1 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhDerive, tmem2T0, tmem2T1);

    const std::string_view texPath =
        mRdp->texture_to_load.raw_tex_metadata.resource != nullptr
            ? GetBaseTexturePath(mRdp->texture_to_load.raw_tex_metadata.resource->GetInitData()->Path)
            : std::string_view{};
    auto maskedTextureIter = mMaskedTextures.find(texPath);
    if (maskedTextureIter != mMaskedTextures.end()) {
        loaded.masked = true;
        loaded.blended = maskedTextureIter->second.replacementData != nullptr;
    } else {
        loaded.masked = false;
        loaded.blended = false;
    }

    const uint64_t tmem2T2 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhPath, tmem2T1, tmem2T2);
    gdx_tmem2_stat[loaded.raw_tex_metadata.resource != nullptr ? kTmem2StResource : kTmem2StRaw]++;

    // LOADBLOCK-OPT same-content skip: menus replay an identical load sequence every frame
    // (82 LOADBLOCKs/frame of unchanged o2r-backed textures on the measured device). When
    // re-running this load would provably reproduce the span, the slots and the TMEM mirror
    // bit-for-bit (see GdxTmemSameContentLoad), skip all three. masked/blended were derived
    // from the LIVE mMaskedTextures above and participate in the equality check, so
    // replacement-pack changes fall through to the full store.
    // [crowd2] The skip no longer arms textures_changed: a proven bit-identical replay
    // leaves no observable state, and the one hazard the blanket arm papered over — a cache
    // eviction nulling the binding while the span stayed live — now arms at the eviction
    // site itself (see TextureCacheLookup/TextureCacheDelete). This is what lets a machine
    // part re-asserting its already-loaded texture keep the draw memo and its batch.
    const bool tmem2Same = kGdxTmemSameContentSkip && GdxTmemSameContentLoad(*mRdp, tmemIndex, 1, loaded);
    const uint64_t tmem2T3 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhCmp, tmem2T2, tmem2T3);
    if (tmem2Same) {
        gdx_tmem2_stat[kTmem2StLbSkips]++;
        GdxTmem2Acc(tmem2On, kTmem2PhTotal, tmem2T0, tmem2T3);
        return;
    }

    // Mirror the block transfer into emulated TMEM so import can decode purely from
    // tile-descriptor state. A straight byte copy is correct: the block load's DXT interleave
    // cancels between a linear write at load time and a linear read at import time.
    uint32_t tmemWriteEndByte = 0; // absolute end of the mirror write, for the skip stamp
    if (loaded.addr != nullptr) {
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIndex) * 8u;
        if (tmemByteOffset < sizeof(mRdp->tmem)) {
            uint32_t copyBytes = std::min<uint32_t>(size_bytes, sizeof(mRdp->tmem) - tmemByteOffset);
            const size_t readable = TmemSourceReadableLimit(loaded.addr);
            if (readable < copyBytes) {
                copyBytes = static_cast<uint32_t>(readable);
            }
            if (copyBytes != 0) {
                memcpy(mRdp->tmem + tmemByteOffset, loaded.addr, copyBytes);
                mRdp->tmem_generation++;
                tmemWriteEndByte = tmemByteOffset + copyBytes;
                gdx_tmem2_stat[kTmem2StMirrorBytes] += copyBytes; // [tmem2]
            }
        }
    }
    const uint64_t tmem2T4 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhMirror, tmem2T3, tmem2T4);

    /* One line per distinct (tmem, siz, lrs, dxt): if a decode's slot sizes disagree with what
       this LOADBLOCK stored, the slot was replaced between load and import. */
    {
        static const bool sDiagFontMachineC = std::getenv("GDX_DIAG_FONT_MACHINE") != nullptr;
        if (sDiagFontMachineC) {
            const uint64_t comboKey = (static_cast<uint64_t>(tmemIndex) << 40) |
                                      (static_cast<uint64_t>(mRdp->texture_to_load.siz) << 32) |
                                      (static_cast<uint64_t>(lrs) << 12) | dxt;
            static uint64_t sSeenCombos[96] = {};
            static int sSeenComboCount = 0;
            bool seen = false;
            for (int s = 0; s < sSeenComboCount; s++) {
                if (sSeenCombos[s] == comboKey) {
                    seen = true;
                    break;
                }
            }
            if (!seen && sSeenComboCount < 96) {
                sSeenCombos[sSeenComboCount++] = comboKey;
                gdx_dbg_logf("[fontmach] loadblock tile=%u tmem=0x%X siz=%u w=%u lrs=%u dxt=%u "
                             "sizeB=%u lineB=%u addr=%p\n",
                             tile, tmemIndex, mRdp->texture_to_load.siz, mRdp->texture_to_load.width,
                             lrs, dxt, loaded.size_bytes, loaded.line_size_bytes,
                             static_cast<const void*>(loaded.addr));
            }
        }
    }

    StoreLoadedTexture(tmemIndex, loaded);
    const uint64_t tmem2T5 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxNoteTmemLoadKind(*mRdp, tmemIndex, 1, tmemWriteEndByte);
    // [crowd2] LOADBLOCK is table-whitelisted now; a real store re-creates the legacy
    // dirty contribution here (the draw-state derivation reads the loaded slot's sizes).
    sGfxS7DirtyMask |= static_cast<uint8_t>(kGfxS7DirtyTri | kGfxS7DirtyRect | kGfxS7DirtyRectViewport);
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
    const uint64_t tmem2T6 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhNote, tmem2T5, tmem2T6);
    GdxTmem2Acc(tmem2On, kTmem2PhTotal, tmem2T0, tmem2T6);
}

void Interpreter::GfxDpLoadTile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    const bool tmem2On = GdxTmem2ProfOn(); // [tmem2] phase timers (gputrace-gated)
    const uint64_t tmem2T0 = GdxTmem2Now(tmem2On);
    gdx_tmem2_stat[kTmem2StLtCalls]++;

    uint32_t word_size_shift = 0;
    switch (mRdp->texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }

    uint32_t offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    uint32_t offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    uint32_t tile_width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t tile_height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t full_image_width = mRdp->texture_to_load.width;

    uint32_t offset_x_in_bytes = offset_x << word_size_shift;
    uint32_t tile_line_size_bytes = tile_width << word_size_shift;
    uint32_t full_image_line_size_bytes = full_image_width << word_size_shift;

    uint32_t orig_size_bytes = tile_line_size_bytes * tile_height;
    uint32_t size_bytes = orig_size_bytes;
    uint32_t start_offset_bytes = full_image_line_size_bytes * offset_y + offset_x_in_bytes;

    float h_byte_scale = mRdp->texture_to_load.raw_tex_metadata.h_byte_scale;
    float v_pixel_scale = mRdp->texture_to_load.raw_tex_metadata.v_pixel_scale;

    if (h_byte_scale != 1 || v_pixel_scale != 1) {
        start_offset_bytes = h_byte_scale * (v_pixel_scale * offset_y * full_image_line_size_bytes + offset_x_in_bytes);
        size_bytes *= h_byte_scale * v_pixel_scale;
        full_image_line_size_bytes *= h_byte_scale;
        tile_line_size_bytes *= h_byte_scale;
    }

    const uint16_t tmemIndex = mRdp->texture_tile[tile].tmem_index;
    LoadedTexture loaded{};
    loaded.orig_size_bytes = orig_size_bytes;
    loaded.size_bytes = size_bytes;
    loaded.full_image_line_size_bytes = full_image_line_size_bytes;
    loaded.line_size_bytes = tile_line_size_bytes;

    //    assert(size_bytes <= 4096 && "bug: too big texture");
    loaded.tex_flags = mRdp->texture_to_load.tex_flags;
    loaded.raw_tex_metadata = mRdp->texture_to_load.raw_tex_metadata;
    loaded.addr = mRdp->texture_to_load.addr != nullptr ? mRdp->texture_to_load.addr + start_offset_bytes : nullptr;
    const uint64_t tmem2T1 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhDerive, tmem2T0, tmem2T1);

    const std::string_view texPath =
        mRdp->texture_to_load.raw_tex_metadata.resource != nullptr
            ? GetBaseTexturePath(mRdp->texture_to_load.raw_tex_metadata.resource->GetInitData()->Path)
            : std::string_view{};
    auto maskedTextureIter = mMaskedTextures.find(texPath);
    if (maskedTextureIter != mMaskedTextures.end()) {
        loaded.masked = true;
        loaded.blended = maskedTextureIter->second.replacementData != nullptr;
    } else {
        loaded.masked = false;
        loaded.blended = false;
    }

    const uint64_t tmem2T2 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhPath, tmem2T1, tmem2T2);
    gdx_tmem2_stat[loaded.raw_tex_metadata.resource != nullptr ? kTmem2StResource : kTmem2StRaw]++;

    // LOADTILE same-content skip, mirroring GfxDpLoadBlock's: the row-strided TMEM write
    // below is a pure function of the payload's line/full-line sizes, tile_height
    // (= orig_size / line_size, both compared) and addr, so payload equality under an
    // armed kind==2 stamp implies a bit-identical re-run. F4 measured 3.15 ms/frame over
    // 11 calls on the same device — the same textures re-loaded every steady menu frame.
    const bool tmem2Same = kGdxTmemSameContentSkip && GdxTmemSameContentLoad(*mRdp, tmemIndex, 2, loaded);
    const uint64_t tmem2T3 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhCmp, tmem2T2, tmem2T3);
    if (tmem2Same) {
        gdx_tmem2_stat[kTmem2StLtSkips]++;
        GdxTmem2Acc(tmem2On, kTmem2PhTotal, tmem2T0, tmem2T3);
        // [crowd2] The load tile's extent writes ride the same value gate as
        // GfxDpSetTileSize; the skip itself no longer arms (see GfxDpLoadBlock).
        const auto& t = mRdp->texture_tile[tile];
        if (!(t.uls == uls && t.ult == ult && t.lrs == lrs && t.lrt == lrt)) {
            mRdp->texture_tile[tile].uls = uls;
            mRdp->texture_tile[tile].ult = ult;
            mRdp->texture_tile[tile].lrs = lrs;
            mRdp->texture_tile[tile].lrt = lrt;
            sGfxS7DirtyMask |=
                static_cast<uint8_t>(kGfxS7DirtyTri | kGfxS7DirtyRect | kGfxS7DirtyRectViewport);
            mRdp->textures_changed[0] = true;
            mRdp->textures_changed[1] = true;
        }
        return;
    }

    // TMEM emulation: mirror the tile sub-rectangle into emulated TMEM row by
    // row. Destination rows advance at the 8-byte-aligned tile line stride,
    // matching how the hardware packs LoadTile rows.
    uint32_t tmemWriteEndByte = 0; // absolute end of the mirror write, for the skip stamp
    if (loaded.addr != nullptr) {
        const uint32_t tmemByteOffset = static_cast<uint32_t>(tmemIndex) * 8u;
        const uint32_t destStride = (tile_line_size_bytes + 7u) & ~7u;
        if (tmemByteOffset < sizeof(mRdp->tmem) && destStride != 0) {
            size_t readable = TmemSourceReadableLimit(loaded.addr);
            bool wrote = false;
            for (uint32_t y = 0; y < tile_height; y++) {
                const uint32_t destOff = tmemByteOffset + y * destStride;
                const size_t srcOff = static_cast<size_t>(y) * full_image_line_size_bytes;
                if (destOff >= sizeof(mRdp->tmem) || srcOff + tile_line_size_bytes > readable) {
                    break;
                }
                const uint32_t rowBytes =
                    std::min<uint32_t>(tile_line_size_bytes, sizeof(mRdp->tmem) - destOff);
                memcpy(mRdp->tmem + destOff, loaded.addr + srcOff, rowBytes);
                wrote = true;
                tmemWriteEndByte = destOff + rowBytes;
                gdx_tmem2_stat[kTmem2StMirrorBytes] += rowBytes; // [tmem2]
            }
            if (wrote) {
                mRdp->tmem_generation++;
            }
        }
    }
    const uint64_t tmem2T4 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhMirror, tmem2T3, tmem2T4);

    StoreLoadedTexture(tmemIndex, loaded);
    const uint64_t tmem2T5 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxNoteTmemLoadKind(*mRdp, tmemIndex, 2, tmemWriteEndByte);
    mRdp->texture_tile[tile].uls = uls;
    mRdp->texture_tile[tile].ult = ult;
    mRdp->texture_tile[tile].lrs = lrs;
    mRdp->texture_tile[tile].lrt = lrt;

    // [crowd2] LOADTILE is table-whitelisted now; a real store re-creates the legacy
    // dirty contribution here (see GfxDpLoadBlock).
    sGfxS7DirtyMask |= static_cast<uint8_t>(kGfxS7DirtyTri | kGfxS7DirtyRect | kGfxS7DirtyRectViewport);
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;
    const uint64_t tmem2T6 = GdxTmem2Now(tmem2On); // [tmem2]
    GdxTmem2Acc(tmem2On, kTmem2PhNote, tmem2T5, tmem2T6);
    GdxTmem2Acc(tmem2On, kTmem2PhTotal, tmem2T0, tmem2T6);
}

/*static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

static void GfxDpSetCombineMode(uint32_t rgb, uint32_t alpha) {
    mRdp->combine_mode = rgb | (alpha << 12);
}*/

void Interpreter::GfxDpSetCombineMode(uint32_t rgb, uint32_t alpha, uint32_t rgb_cyc2, uint32_t alpha_cyc2) {
    mRdp->combine_mode = rgb | (alpha << 16) | ((uint64_t)rgb_cyc2 << 28) | ((uint64_t)alpha_cyc2 << 44);
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13);
}

static inline uint32_t alpha_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9);
}

// Sign-extend a 9-bit value (used for G_SETCONVERT K0..K5).
static inline int16_t sign_extend_9(uint32_t v) {
    return (int16_t)((v & 0x100) ? (int32_t)(v | 0xFFFFFE00u) : (int32_t)v);
}

void Interpreter::GfxDpSetGrayscaleColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    mRdp->grayscale_color.r = r;
    mRdp->grayscale_color.g = g;
    mRdp->grayscale_color.b = b;
    mRdp->grayscale_color.a = a;
}

void Interpreter::GfxDpSetEnvColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifdef __3DS__
    // Split the batch on a VALUE change: the citro3d backend binds this colour as a
    // TexEnv stage constant from vertex 0 of the flushed batch, so tris appended
    // under the old value must be drawn before the register can move (see the
    // gdx3ds_prim_note_set comment above / lus-3ds-primenv-flush.patch README).
    if (mRdp->env_color.r != r || mRdp->env_color.g != g || mRdp->env_color.b != b ||
        mRdp->env_color.a != a) {
        const unsigned pendingTris = (unsigned)mBufVboNumTris;
        if (mBufVboLen > 0) {
            if (GdxHudRegFoldable(false)) { // [native] fx_hud: env rides per vertex in this batch
                sGdxHudFoldEnv++;
                sGdxPackFolded = true;
            } else {
                Flush();
            }
        }
        gdx3ds_prim_note_set(0, r, g, b, a, pendingTris, mBufVboLen == 0 && pendingTris > 0 ? 1 : 0);
    }
#endif
    mRdp->env_color.r = r;
    mRdp->env_color.g = g;
    mRdp->env_color.b = b;
    mRdp->env_color.a = a;
}

void Interpreter::GfxDpSetPrimColor(uint8_t m, uint8_t l, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifdef __3DS__
    // Same batch split as GfxDpSetEnvColor. prim_lod_fraction participates: it rides
    // the same stage-constant path (G_CCMUX_PRIM_LOD_FRAC / G_ACMUX_PRIM_LOD_FRAC).
    if (mRdp->prim_color.r != r || mRdp->prim_color.g != g || mRdp->prim_color.b != b ||
        mRdp->prim_color.a != a || mRdp->prim_lod_fraction != l) {
        const unsigned pendingTris = (unsigned)mBufVboNumTris;
        if (mBufVboLen > 0) {
            if (GdxHudRegFoldable(true)) { // [native] fx_hud: prim rides per vertex in this batch
                sGdxHudFoldPrim++;
                sGdxPackFolded = true;
            } else {
                Flush();
            }
        }
        gdx3ds_prim_note_set(1, r, g, b, a, pendingTris, mBufVboLen == 0 && pendingTris > 0 ? 1 : 0);
    }
#endif
    mRdp->prim_lod_fraction = l;
    mRdp->prim_color.r = r;
    mRdp->prim_color.g = g;
    mRdp->prim_color.b = b;
    mRdp->prim_color.a = a;
}

void Interpreter::GfxDpSetFogColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifdef __3DS__
    // [traffic] Same batch-split-on-value-change guard as GfxDpSetPrimColor/GfxDpSetEnvColor
    // (lus-3ds-primenv-flush): the 3DS backend hoists the fog blend stage's colour from
    // VERTEX 0 of the flushed batch (RefreshStageConstants fogBlendStage) and the per-vertex
    // factor for G_BL_CLR_BL draws samples fog_color.a at append time — so a batch spanning a
    // fog-colour change binds the OLD colour to every triangle. The gDPPipeSync drain used to
    // mask exactly this hazard (game DLs pair gDPSetFogColor with a preceding pipesync); with
    // the pipesync a no-op (gfx_rdp_pipe_sync_handler_rdp) the guard must live here. No-op
    // re-sets do not split. Desktop interpolates the per-vertex baked values and needs none.
    if (mRdp->fog_color.r != r || mRdp->fog_color.g != g || mRdp->fog_color.b != b ||
        mRdp->fog_color.a != a) {
        Flush();
    }
#endif
    mRdp->fog_color.r = r;
    mRdp->fog_color.g = g;
    mRdp->fog_color.b = b;
    mRdp->fog_color.a = a;
}

void Interpreter::GfxDpSetBlendColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    mRdp->blend_color.r = r;
    mRdp->blend_color.g = g;
    mRdp->blend_color.b = b;
    mRdp->blend_color.a = a;
}

void Interpreter::GfxDpSetFillColor(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    mRdp->fill_color.r = SCALE_5_8(r);
    mRdp->fill_color.g = SCALE_5_8(g);
    mRdp->fill_color.b = SCALE_5_8(b);
    mRdp->fill_color.a = a * 255;
}

void Interpreter::GfxDrawRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = mRdp->other_mode_h;
    uint32_t cycle_type = (mRdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (cycle_type == G_CYC_COPY) {
        mRdp->other_mode_h = (mRdp->other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH(mActiveFrameBuffer)) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT(mActiveFrameBuffer))) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH(mActiveFrameBuffer)) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT(mActiveFrameBuffer))) + 1.0f;

    {
        const bool fixedAspectFramebuffer =
            mFbActive && mActiveFrameBuffer != mFrameBuffers.end() &&
            (!mActiveFrameBuffer->second.resize || mActiveFrameBuffer->second.forceFixedAspect);
        const float currentAspect = (float)mCurDimensions.width / (float)mCurDimensions.height;
        const float aspectScale = (4.0f / 3.0f) / currentAspect;
        const uint32_t widescreenMode =
            mRsp->extra_geometry_mode &
            (G_EX_WIDESCREEN_STRETCH | G_EX_WIDESCREEN_ANCHOR_LEFT | G_EX_WIDESCREEN_ANCHOR_RIGHT |
             G_EX_WIDESCREEN_DISTRIBUTE);
        // CVar states are latched per frame in StartFrame. Forced-4:3 frames composite into a
        // centred pillarbox, so a stretch or anchor scope that slipped into such a frame would be
        // double-corrected afterwards; the scopes go inert with the frame.
        const bool widescreenFrameActive = !fixedAspectFramebuffer && currentAspect > (4.0f / 3.0f) &&
                                           mWidescreenEnabledCache && !mForceFixedAspectCache;
        // STRETCH follows the 3D Widescreen CVar alone: transitions redraw a captured widescreen
        // frame and must cover the full viewport whenever the live frame renders widescreen.
        // ANCHOR/DISTRIBUTE reposition individual 2D elements and stay behind the WidescreenUI
        // opt-in.
        const bool stretchActive = widescreenFrameActive && (widescreenMode & G_EX_WIDESCREEN_STRETCH) != 0;
        const bool enhancedWidescreenUi = widescreenFrameActive && widescreenMode != 0 && mWidescreenUiCache;

        if (stretchActive) {
            // F-Zero X's fullscreen 2D safe area is x=12..308. Map that explicit draw scope to
            // the true viewport edges; foreground/menu artwork remains on the normal 4:3 path.
            constexpr float kSafeAreaScale = 160.0f / (160.0f - 12.0f);
            ulxf *= kSafeAreaScale;
            lrxf *= kSafeAreaScale;
        } else if (enhancedWidescreenUi && (widescreenMode & G_EX_WIDESCREEN_DISTRIBUTE) != 0) {
            // Place a 2D widget by its native 320-wide centre so it follows a widescreen-spread
            // 3D viewport, but retain the widget's 4:3-corrected width. This is distinct from
            // STRETCH: SELECT MACHINE's cursor must move with its six 3D columns without becoming
            // horizontally distorted.
            const float center = (ulxf + lrxf) * 0.5f;
            const float halfWidth = (lrxf - ulxf) * 0.5f * aspectScale;
            ulxf = center - halfWidth;
            lrxf = center + halfWidth;
        } else {
            ulxf = AdjXForAspectRatio(ulxf);
            lrxf = AdjXForAspectRatio(lrxf);

            if (enhancedWidescreenUi) {
                const bool anchorLeft = (widescreenMode & G_EX_WIDESCREEN_ANCHOR_LEFT) != 0;
                const bool anchorRight = (widescreenMode & G_EX_WIDESCREEN_ANCHOR_RIGHT) != 0;
                if (anchorLeft != anchorRight) {
                    // Clamp anchor travel to a centred band of aspect min(current, HudMaxAspect)
                    // rather than the raw viewport edge, which parks HUD elements at the physical
                    // corners of arbitrarily wide displays. The band edge sits at NDC
                    // uiAspect/currentAspect, so travel is (uiAspect - 4/3)/currentAspect --
                    // exactly (1 - aspectScale), the old formula, for every aspect <=
                    // HudMaxAspect.
                    const float currentAspectForUi =
                        (float)mCurDimensions.width / (float)mCurDimensions.height;
                    const float uiAspect = std::min(currentAspectForUi, mHudMaxAspectCache);
                    float offset = (anchorRight ? 1.0f : -1.0f) *
                                   ((uiAspect - (4.0f / 3.0f)) / currentAspectForUi);
                    if (mRemoveBordersCache) {
                        // HUD elements are authored 12 px inside the overscan-safe frame; with the
                        // frame removed, extend anchor travel by that margin (12/160 in 4:3 NDC)
                        // so they glue to the true corners instead of the retired safe edge.
                        offset += (anchorRight ? 1.0f : -1.0f) * (0.1f / currentAspectForUi);
                    }
                    ulxf += offset;
                    lrxf += offset;
                }
            }
        }
    }

    struct LoadedVertex* ul = &mRsp->loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &mRsp->loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &mRsp->loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &mRsp->loaded_vertices[MAX_VERTICES + 3];

    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;

    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;

    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;

    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;

    // The coordinates for texture rectangle shall bypass the viewport setting.
    // [texrect] The adjusted bypass viewport is derived once per rect RUN, not per rect: its
    // inputs only move through non-whitelisted opcodes or task/frame boundaries, all of which
    // arm kGfxS7DirtyRectViewport (see the enum). The recompute branch is byte-identical to
    // the legacy always-on derivation, including the AdjustVIewportOrScissor virtual
    // GetClipParameters round trip it performs.
    struct XYWidthHeight default_viewport;
    if ((sGfxS7DirtyMask & kGfxS7DirtyRectViewport) != 0) {
        if (!mFbActive) {
            default_viewport = { 0, (int16_t)mNativeDimensions.height, mNativeDimensions.width,
                                 mNativeDimensions.height };
        } else {
            default_viewport = { 0, (int16_t)mActiveFrameBuffer->second.orig_height,
                                 mActiveFrameBuffer->second.orig_width, mActiveFrameBuffer->second.orig_height };
        }
        AdjustVIewportOrScissor(&default_viewport);
        sGfxRectAdjustedViewport = default_viewport;
        sGfxS7DirtyMask &= static_cast<uint8_t>(~kGfxS7DirtyRectViewport);
    } else {
        default_viewport = sGfxRectAdjustedViewport;
    }

    struct XYWidthHeight viewport_saved = mRdp->viewport;
    uint32_t geometry_mode_saved = mRsp->geometry_mode;

    mRdp->viewport = default_viewport;
    mRdp->viewport_or_scissor_changed = true;
    mRsp->geometry_mode = 0;

    GfxSpTri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3, true);
    GfxSpTri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3, true);

    mRsp->geometry_mode = geometry_mode_saved;
    mRdp->viewport = viewport_saved;
    mRdp->viewport_or_scissor_changed = true;

    if (cycle_type == G_CYC_COPY) {
        mRdp->other_mode_h = saved_other_mode_h;
    }
    // [perf-s7] the temporary othermode/viewport/geometry scope above ran draws through
    // GfxSpTri1; whatever sampler/render state they left behind must not be carried into
    // the next game triangle's memo fast path. Tri memo ONLY: the rect memo (banked by the
    // draws above) stays valid — it describes exactly this scope's state, and its own dirty
    // bit tracks everything that could invalidate it between rect runs.
    GfxS7InvalidateTriMemo();
}

void Interpreter::GfxDpTextureRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                                        int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    // printf("render %d at %d\n", tile, lrx);
    uint64_t saved_combine_mode = mRdp->combine_mode;
    if ((mRdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1, but only when the game followed the 4x convention. F-Zero X emits
        // dsdx=0x0400 (already 1.0 texel/pixel) in copy mode, where dividing would give a 4x zoom,
        // so skip the shift when the magnitude is already at or below 1.0.
        if (dsdx > 0x0400 || dsdx < -0x0400) {
            dsdx >>= 2;
        }

        // Color combiner is turned off in copy mode
        GfxDpSetCombineMode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), alpha_comb(0, 0, 0, G_ACMUX_TEXEL0), 0, 0);

        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;

    LoadedVertex* ul = &mRsp->loaded_vertices[MAX_VERTICES + 0];
    LoadedVertex* ll = &mRsp->loaded_vertices[MAX_VERTICES + 1];
    LoadedVertex* lr = &mRsp->loaded_vertices[MAX_VERTICES + 2];
    LoadedVertex* ur = &mRsp->loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }

    uint8_t saved_tile = mRdp->first_tile_index;
    if (saved_tile != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }
    mRdp->first_tile_index = tile;

#ifdef __3DS__
    // [trectbatch] atlas eligibility of this rect, consumed by GfxSpTri1's import block.
    sTrectRectAtlasOk = sTrectBatchOn && GdxTrectRectEligible(mRdp, tile, uls, ult, lrs, lrt);
    // [trect] census bracket (gated; see the block above GfxS7InvalidateDrawMemos).
    const bool trectCensus = GdxTrectActive();
    long long trectT0 = 0;
    uint64_t trectImpT0 = 0;
    uint32_t trectImpC0 = 0;
    if (trectCensus) {
        GdxTrectEnter(mRdp, mRsp, tile, uls, ult, lrs, lrt, saved_combine_mode);
        trectT0 = gdx3ds_prof_now();
        trectImpT0 = gdx3ds_prof_sec_ticks[kTrectProfImp];
        trectImpC0 = gdx3ds_prof_sec_calls[kTrectProfImp];
        sTrectInRect = true;
    }
#endif
    GfxDrawRectangle(ulx, uly, lrx, lry);
#ifdef __3DS__
    sTrectRectAtlasOk = false;
    if (trectCensus) {
        sTrectInRect = false;
        sTrect.rectTicks += (uint64_t)(gdx3ds_prof_now() - trectT0);
        sTrect.impTicks += gdx3ds_prof_sec_ticks[kTrectProfImp] - trectImpT0;
        sTrect.impCalls += gdx3ds_prof_sec_calls[kTrectProfImp] - trectImpC0;
        GdxTrectExit();
    }
#endif
    if (saved_tile != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }
    mRdp->first_tile_index = saved_tile;
    mRdp->combine_mode = saved_combine_mode;
}

void Interpreter::GfxDpImageRectangle(int32_t tile, int32_t w, int32_t h, int32_t ulx, int32_t uly, int16_t uls,
                                      int16_t ult, int32_t lrx, int32_t lry, int16_t lrs, int16_t lrt) {

    LoadedVertex* ul = &mRsp->loaded_vertices[MAX_VERTICES + 0];
    LoadedVertex* ll = &mRsp->loaded_vertices[MAX_VERTICES + 1];
    LoadedVertex* lr = &mRsp->loaded_vertices[MAX_VERTICES + 2];
    LoadedVertex* ur = &mRsp->loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls * 32;
    ul->v = ult * 32;
    lr->u = lrs * 32;
    lr->v = lrt * 32;
    ll->u = uls * 32;
    ll->v = lrt * 32;
    ur->u = lrs * 32;
    ur->v = ult * 32;

    // ensure we have the correct texture size, format and starting position
    mRdp->texture_tile[tile].siz = G_IM_SIZ_8b;
    mRdp->texture_tile[tile].fmt = G_IM_FMT_RGBA;
    mRdp->texture_tile[tile].cms = 0;
    mRdp->texture_tile[tile].cmt = 0;
    mRdp->texture_tile[tile].shifts = 0;
    mRdp->texture_tile[tile].shiftt = 0;
    mRdp->texture_tile[tile].uls = 0 * 4;
    mRdp->texture_tile[tile].ult = 0 * 4;
    mRdp->texture_tile[tile].lrs = w * 4;
    mRdp->texture_tile[tile].lrt = h * 4;
    mRdp->texture_tile[tile].line_size_bytes = w << (mRdp->texture_tile[tile].siz >> 1);

    // In-place mutation of the current slot: goes through the materializing write accessor
    // so the whole containing span is physically consistent before (and after) the edit.
    auto& loadtex = mRdp->MutableLoadedTextureAt(mRdp->texture_tile[tile].tmem_index);
    loadtex.full_image_line_size_bytes = loadtex.line_size_bytes = mRdp->texture_tile[tile].line_size_bytes;
    loadtex.size_bytes = loadtex.orig_size_bytes = loadtex.line_size_bytes * h;

    // [crowd2] This handler rewrites the tile descriptor AND the loaded-slot payload in
    // place (above), without going through GfxDpSetTile/StoreLoadedTexture. The next game
    // SETTILE may compare equal against the game's own values and skip its re-arm, so the
    // mutation site itself must arm — unconditionally, not just on a tile switch.
    mRdp->textures_changed[0] = true;
    mRdp->textures_changed[1] = true;

    uint8_t saved_tile = mRdp->first_tile_index;
    mRdp->first_tile_index = tile;

    GfxDrawRectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        mRdp->textures_changed[0] = true;
        mRdp->textures_changed[1] = true;
    }
    mRdp->first_tile_index = saved_tile;
}

void Interpreter::GfxDpFillRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (mRdp->color_image_address == mRdp->z_buf_address) {
        // Fullscreen Z clears are redundant — already done by glClear at frame start.
        bool isFullScreen = (ulx <= 0 && uly <= 0 && lrx >= (int32_t)(mNativeDimensions.width - 1) * 4 &&
                             lry >= (int32_t)(mNativeDimensions.height - 1) * 4);
        if (isFullScreen) {
            return;
        }

        // Partial depth clear (e.g. HUD model regions): clear the actual depth buffer
        // via a scissored depth clear instead of drawing a colored rect to the color buffer.
        Flush();

        // Convert U10.2 coords to pixel coords and add +1 pixel for fill mode
        int32_t expanded_lrx = lrx + (1 << 2);
        int32_t expanded_lry = lry + (1 << 2);
        float x = ulx / 4.0f;
        float y = expanded_lry / 4.0f;
        float w = (expanded_lrx - ulx) / 4.0f;
        float h = (expanded_lry - uly) / 4.0f;

        struct XYWidthHeight area;
        area.x = (int16_t)x;
        area.y = (int16_t)y;
        area.width = (uint32_t)w;
        area.height = (uint32_t)h;
        AdjustVIewportOrScissor(&area);

        mRapi->ClearDepthRegion(area.x, area.y, area.width, area.height);
        return;
    }
    uint32_t mode = (mRdp->other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    // Hardware draws nothing for an inverted rect pair: the span walker yields a
    // negative scanline count. F-Zero X hits this when a scrolled rect's 10-bit
    // coordinate wraps negative (records_entry.c via ending.c) — do not "fix" this
    // into a clamp, which would rasterise a full-height stripe the hardware never
    // drew. Strict '>': an equal pair is a legitimate 1-pixel rect. Placed before
    // the +1 expansion below so the comparison uses the values the RDP compares.
    if (ulx > lrx || uly > lry) {
        return;
    }

    // Expand fullscreen fill rects to cover widescreen viewports.
    // Without this, screen clears and fades only cover the native 4:3 area.
    if (ulx == 0 && uly == 0) {
        bool isFullScreen = (lrx == ((int32_t)(mNativeDimensions.width - 1) * 4) &&
                             lry == ((int32_t)(mNativeDimensions.height - 1) * 4));
        if (isFullScreen) {
            // The fixed +-1024/2048 extents cover NDC ~+-2.2 after hor+ compression, which stops
            // covering the frame above aspect ~2.93 (unfaded side bands at 32:9). Derive the
            // horizontal extent from the live aspect instead, floored at the old constants:
            // anything wider than the screen is scissored identically, so 16:9-and-below output
            // is unchanged.
            const float fsAspect = (float)mCurDimensions.width / (float)mCurDimensions.height;
            const float fsK = (4.0f / 3.0f) / std::max(fsAspect, 4.0f / 3.0f);
            int32_t ex = (int32_t)(160.0f * (1.0f / std::max(fsK, 0.05f) + 1.0f) * 4.0f) + 64;
            if (ex < 2048) {
                ex = 2048;
            }
            ulx = -ex;
            uly = -1024;
            lrx = ex;
            lry = 2048;
        }
    }

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        LoadedVertex* v = &mRsp->loaded_vertices[i];
        v->color = mRdp->fill_color;
    }

    uint64_t saved_combine_mode = mRdp->combine_mode;

    if (mode == G_CYC_FILL) {
        GfxDpSetCombineMode(color_comb(0, 0, 0, G_CCMUX_SHADE), alpha_comb(0, 0, 0, G_ACMUX_SHADE), 0, 0);
    }

    GfxDrawRectangle(ulx, uly, lrx, lry);
    mRdp->combine_mode = saved_combine_mode;
}

void Interpreter::GfxDpSetZImage(void* zBufAddr) {
    mRdp->z_buf_address = zBufAddr;
}

void Interpreter::GfxDpSetColorImage(uint32_t format, uint32_t size, uint32_t width, void* address) {
    mRdp->color_image_address = address;
}

void Interpreter::GfxSpSetOtherMode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = mRdp->other_mode_l | ((uint64_t)mRdp->other_mode_h << 32);
    om = (om & ~mask) | mode;
    mRdp->other_mode_l = (uint32_t)om;
    mRdp->other_mode_h = (uint32_t)(om >> 32);
}

void Interpreter::GfxDpSetOtherMode(uint32_t h, uint32_t l) {
    mRdp->other_mode_h = h;
    mRdp->other_mode_l = l;
}

void Interpreter::Gfxs2dexBgCopy(F3DuObjBg* bg) {
    /*
    bg->b.imageX = 0;
    bg->b.imageW = width * 4;
    bg->b.frameX = frameX * 4;
    bg->b.imageY = 0;
    bg->b.imageH = height * 4;
    bg->b.frameY = frameY * 4;
    bg->b.imagePtr = source;
    bg->b.imageLoad = G_BGLT_LOADTILE;
    bg->b.imageFmt = fmt;
    bg->b.imageSiz = siz;
    bg->b.imagePal = 0;
    bg->b.imageFlip = 0;
    */

    uintptr_t data = (uintptr_t)bg->b.imagePtr;

    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetadata = {};

    if ((bool)gfx_check_image_signature((char*)data)) {
        std::shared_ptr<Fast::Texture> tex = std::static_pointer_cast<Fast::Texture>(
            Ship::Context::GetInstance()->GetResourceManager()->LoadResourceProcess((char*)data));
        texFlags = tex->Flags;
        rawTexMetadata.width = tex->Width;
        rawTexMetadata.height = tex->Height;
        rawTexMetadata.h_byte_scale = tex->HByteScale;
        rawTexMetadata.v_pixel_scale = tex->VPixelScale;
        rawTexMetadata.type = tex->Type;
        rawTexMetadata.resource = tex;
        data = (uintptr_t) reinterpret_cast<char*>(tex->ImageData);
    }

    s16 dsdx = 4 << 10;
    s16 uls = bg->b.imageX << 3;
    // Flip flag only flips horizontally
    if (bg->b.imageFlip == G_BG_FLAG_FLIPS) {
        dsdx = -dsdx;
        uls = (bg->b.imageW - bg->b.imageX) << 3;
    }

    SUPPORT_CHECK(bg->b.imageSiz == G_IM_SIZ_16b);
    GfxDpSetTextureImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, nullptr, texFlags, rawTexMetadata, (void*)data);
    GfxDpSetTile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
    GfxDpLoadBlock(G_TX_LOADTILE, 0, 0, (bg->b.imageW * bg->b.imageH >> 4) - 1, 0);
    GfxDpSetTile(bg->b.imageFmt, G_IM_SIZ_16b, bg->b.imageW >> 4, 0, G_TX_RENDERTILE, bg->b.imagePal, 0, 0, 0, 0, 0, 0);
    GfxDpSetTileSize(G_TX_RENDERTILE, 0, 0, bg->b.imageW, bg->b.imageH);
    GfxDpTextureRectangle(bg->b.frameX, bg->b.frameY, bg->b.frameX + bg->b.imageW - 4, bg->b.frameY + bg->b.imageH - 4,
                          G_TX_RENDERTILE, uls, bg->b.imageY << 3, dsdx, 1 << 10, false);
}

void Interpreter::Gfxs2dexBg1cyc(F3DuObjBg* bg) {
    uintptr_t data = (uintptr_t)bg->b.imagePtr;

    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetadata = {};

    if ((bool)gfx_check_image_signature((char*)data)) {
        std::shared_ptr<Fast::Texture> tex = std::static_pointer_cast<Fast::Texture>(
            Ship::Context::GetInstance()->GetResourceManager()->LoadResourceProcess((char*)data));
        texFlags = tex->Flags;
        rawTexMetadata.width = tex->Width;
        rawTexMetadata.height = tex->Height;
        rawTexMetadata.h_byte_scale = tex->HByteScale;
        rawTexMetadata.v_pixel_scale = tex->VPixelScale;
        rawTexMetadata.type = tex->Type;
        rawTexMetadata.resource = tex;
        data = (uintptr_t) reinterpret_cast<char*>(tex->ImageData);
    }

    // TODO: Implement bg scaling correctly
    s16 uls = bg->b.imageX >> 2;
    s16 lrs = bg->b.imageW >> 2;

    s16 dsdxRect = 1 << 10;
    s16 ulsRect = bg->b.imageX << 3;
    // Flip flag only flips horizontally
    if (bg->b.imageFlip == G_BG_FLAG_FLIPS) {
        dsdxRect = -dsdxRect;
        ulsRect = (bg->b.imageW - bg->b.imageX) << 3;
    }

    GfxDpSetTextureImage(bg->b.imageFmt, bg->b.imageSiz, bg->b.imageW >> 2, nullptr, texFlags, rawTexMetadata,
                         (void*)data);
    GfxDpSetTile(bg->b.imageFmt, bg->b.imageSiz, 0, 0, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
    GfxDpLoadBlock(G_TX_LOADTILE, 0, 0, (bg->b.imageW * bg->b.imageH >> 4) - 1, 0);
    GfxDpSetTile(bg->b.imageFmt, bg->b.imageSiz, (((lrs - uls) * bg->b.imageSiz) + 7) >> 3, 0, G_TX_RENDERTILE,
                 bg->b.imagePal, 0, 0, 0, 0, 0, 0);
    GfxDpSetTileSize(G_TX_RENDERTILE, 0, 0, bg->b.imageW, bg->b.imageH);

    GfxDpTextureRectangle(bg->b.frameX, bg->b.frameY, bg->b.frameW, bg->b.frameH, G_TX_RENDERTILE, ulsRect,
                          bg->b.imageY << 3, dsdxRect, 1 << 10, false);
}

void Interpreter::Gfxs2dexRecyCopy(F3DuObjSprite* spr) {
    s16 dsdx = 4 << 10;
    [[maybe_unused]] s16 uls = spr->s.objX << 3;
    // Flip flag only flips horizontally
    if (spr->s.imageFlags == G_BG_FLAG_FLIPS) {
        dsdx = -dsdx;
        uls = (spr->s.imageW - spr->s.objX) << 3;
    }

    int realX = spr->s.objX >> 2;
    int realY = spr->s.objY >> 2;
    int realW = (((spr->s.imageW)) >> 5);
    int realH = (((spr->s.imageH)) >> 5);
    float realSW = spr->s.scaleW / 1024.0f;
    float realSH = spr->s.scaleH / 1024.0f;

    int testX = (realX + (realW / realSW));
    int testY = (realY + (realH / realSH));

    GfxDpTextureRectangle(realX << 2, realY << 2, testX << 2, testY << 2, G_TX_RENDERTILE,
                          (s32)mRdp->texture_tile[0].uls << 3, (s32)mRdp->texture_tile[0].ult << 3,
                          (float)(1 << 10) * realSW, (float)(1 << 10) * realSH, false);
}

void* Interpreter::SegAddr(uintptr_t w1) {
    // Segmented?
    if (w1 & 1) {
        uint32_t segNum = (uint32_t)(w1 >> 24);

        uint32_t offset = w1 & 0x00FFFFFE;

        if (mSegmentPointers[segNum] != 0) {
            return (void*)(mSegmentPointers[segNum] + offset);
        } else {
            return (void*)w1;
        }
    } else {
        return (void*)w1;
    }
}

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

void GfxExecStack::start(F3DGfx* dlist) {
    while (!cmd_stack.empty())
        cmd_stack.pop();
    gfx_path.clear();
    cmd_stack.push(dlist);
    disp_stack.clear();
}

void GfxExecStack::stop() {
    while (!cmd_stack.empty())
        cmd_stack.pop();
    gfx_path.clear();
}

F3DGfx*& GfxExecStack::currCmd() {
    return cmd_stack.top();
}

void GfxExecStack::openDisp(const char* file, int line) {
    disp_stack.push_back({ file, line });
}
void GfxExecStack::closeDisp() {
    disp_stack.pop_back();
}
const std::vector<GfxExecStack::CodeDisp>& GfxExecStack::getDisp() const {
    return disp_stack;
}

void GfxExecStack::branch(F3DGfx* caller) {
    F3DGfx* old = cmd_stack.top();
    cmd_stack.pop();
    cmd_stack.push(nullptr);
    cmd_stack.push(old);

    gfx_path.push_back(caller);
}

void GfxExecStack::call(F3DGfx* caller, F3DGfx* callee) {
    cmd_stack.push(callee);
    gfx_path.push_back(caller);
}

F3DGfx* GfxExecStack::ret() {
    F3DGfx* cmd = cmd_stack.top();

    cmd_stack.pop();
    if (!gfx_path.empty()) {
        gfx_path.pop_back();
    }

    while (cmd_stack.size() > 0 && cmd_stack.top() == nullptr) {
        cmd_stack.pop();
        if (!gfx_path.empty()) {
            gfx_path.pop_back();
        }
    }
    return cmd;
}

void gfx_set_framebuffer(int fb, float noise_scale);
void gfx_reset_framebuffer();
void gfx_copy_framebuffer(int fb_dst_id, int fb_src_id, bool copyOnce, bool* hasCopiedPtr);

// The main type of the handler function. These function will take a pointer to a pointer to a Gfx. It needs to be a
// double pointer because we sometimes need to increment and decrement the underlying pointer Returns false if the
// current opcode should be incremented after the handler ends.
typedef bool (*GfxOpcodeHandlerFunc)(F3DGfx** gfx);

bool gfx_load_ucode_handler_f3dex2(F3DGfx** cmd) {
    Interpreter* gfx = sGfxInstanceRaw;
    gfx->mRsp->fog_mul = 0;
    gfx->mRsp->fog_offset = 0;
#ifdef __3DS__
    gdx3ds_fog_note_params(0, 0, 1);
#endif
    return false;
}

bool gfx_cull_dl_handler_f3dex2(F3DGfx** cmd) {
    // TODO:
    return false;
}

bool gfx_marker_handler_otr(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    (*cmd0)++;
    F3DGfx* cmd = (*cmd0);
    gfx->mMarkerOn = true;
    return false;
}

bool gfx_invalidate_tex_cache_handler_f3dex2(F3DGfx** cmd) {
    Interpreter* gfx = sGfxInstanceRaw;
    const uintptr_t texAddr = (*cmd)->words.w1;

    if (texAddr == 0) {
        gfx->TextureCacheClear();
    } else {
        gfx->TextureCacheDelete((const uint8_t*)texAddr);
    }
    return false;
}

bool gfx_noop_handler_f3dex2(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    const char* filename = (const char*)(cmd)->words.w1;
    uint32_t p = C0(16, 8);
    uint32_t l = C0(0, 16);
    if (p == 7) {
        g_exec_stack.openDisp(filename, l);
    } else if (p == 8) {
        if (g_exec_stack.disp_stack.size() == 0) {
            SPDLOG_WARN("CLOSE_DISPS without matching open {}:{}", p, l);
        } else {
            g_exec_stack.closeDisp();
        }
#ifdef __3DS__
    } else if (p == 0x5A || p == 0x5B || p == 0x60) {
        // [native] track: the course tessellator's material-run marker (p=0x5A end, 0x5B
        // verify start, 0x60 [native] track_retained end -- C5 Stage 3). The port replays the recorded VTX/MODIFYVTX/template ops here, at
        // the exact display-list position the stock commands occupied.
        if (&gdx3ds_track_marker != nullptr) {
            gdx3ds_track_marker(sGfxInstanceRaw, (uint32_t)cmd->words.w0, (uintptr_t)cmd->words.w1);
        }
    } else if (p == 0x5C || p == 0x5D) {
        // [native] fx: Racer_Draw's effects-bracket run marker (p=0x5C end, 0x5D verify start),
        // same shape as the track marker; the port replays the recorded sprite ops here.
        if (&gdx3ds_fx_marker != nullptr) {
            gdx3ds_fx_marker(sGfxInstanceRaw, (uint32_t)cmd->words.w0, (uintptr_t)cmd->words.w1);
        }
#endif
    }
    return false;
}

bool gfx_mtx_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    uintptr_t mtxAddr = cmd->words.w1;

    gfx->GfxSpMatrix(C0(0, 8) ^ F3DEX2_G_MTX_PUSH, (const int32_t*)gfx->SegAddr(mtxAddr));
    return false;
}
// Seems to be the same for all other non F3DEX2 microcodes...
bool gfx_mtx_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    uintptr_t mtxAddr = cmd->words.w1;

    gfx->GfxSpMatrix(C0(16, 8), (const int32_t*)gfx->SegAddr(cmd->words.w1));
    return false;
}

bool gfx_mtx_otr_filepath_handler_custom_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    const char* fileName = (const char*)cmd->words.w1;
    const int32_t* mtx = (const int32_t*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(
        (const char*)fileName);

    if (mtx != NULL) {
        gfx->GfxSpMatrix(C0(0, 8) ^ F3DEX2_G_MTX_PUSH, mtx);
    }

    return false;
}

bool gfx_mtx_otr_filepath_handler_custom_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    const char* fileName = (const char*)cmd->words.w1;
    const int32_t* mtx = (const int32_t*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(
        (const char*)fileName);

    if (mtx != NULL) {
        gfx->GfxSpMatrix(C0(16, 8), mtx);
    }

    return false;
}

bool gfx_mtx_otr_filepath_handler_custom(F3DGfx** cmd0) {
    if (ucode_handler_index == ucode_f3dex2) {
        return gfx_mtx_otr_filepath_handler_custom_f3dex2(cmd0);
    } else {
        return gfx_mtx_otr_filepath_handler_custom_f3d(cmd0);
    }
}

bool gfx_mtx_otr_handler_custom_f3dex2(F3DGfx** cmd0) {
    (*cmd0)++;
    F3DGfx* cmd = *cmd0;

    const uint64_t hash = ((uint64_t)cmd->words.w0 << 32) + cmd->words.w1;
    const int32_t* mtx =
        (const int32_t*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(hash);

    if (mtx != NULL) {
        Interpreter* gfx = sGfxInstanceRaw;
        cmd--;
        gfx->GfxSpMatrix(C0(0, 8) ^ F3DEX2_G_MTX_PUSH, mtx);
        cmd++;
    }

    return false;
}

bool gfx_mtx_otr_handler_custom_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    (*cmd0)++;
    F3DGfx* cmd = *cmd0;

    const uint64_t hash = ((uint64_t)cmd->words.w0 << 32) + cmd->words.w1;
    const int32_t* mtx =
        (const int32_t*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(hash);
    if (mtx != nullptr) {
        cmd--;
        gfx->GfxSpMatrix(C0(16, 8), mtx);
        cmd++;
    }
    return false;
}

bool gfx_mtx_otr_handler_custom(F3DGfx** cmd0) {
    if (ucode_handler_index == ucode_f3dex2) {
        return gfx_mtx_otr_handler_custom_f3dex2(cmd0);
    } else {
        return gfx_mtx_otr_handler_custom_f3d(cmd0);
    }
}

bool gfx_pop_mtx_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpPopMatrix((uint32_t)(cmd->words.w1 / 64));

    return false;
}

bool gfx_pop_mtx_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpPopMatrix(1);

    return false;
}

bool gfx_movemem_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpMovememF3dex2(C0(0, 8), C0(8, 8) * 8, gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_dma_io_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    const bool write = C0(23, 1) != 0;
    const uint16_t dmem = static_cast<uint16_t>(C0(13, 10) * 8);
    const size_t size = static_cast<size_t>(C0(0, 12)) + 1;
    gfx->GfxSpDmaIo(write, dmem, gfx->SegAddr(cmd->words.w1), size);
    return false;
}

bool gfx_movemem_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpMovememF3d(C0(16, 8), 0, gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_movemem_handler_otr(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    const uint8_t index = C1(24, 8);
    const uint8_t offset = C1(16, 8);
    const uint8_t hasOffset = C1(8, 8);

    (*cmd0)++;

    const uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (*cmd0)->words.w1;

    if (ucode_handler_index == ucode_f3dex2) {
        gfx->GfxSpMovememF3dex2(index, offset,
                                Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(hash));
    } else {
        auto light = (Fast::LightEntry*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(hash);
        uintptr_t data = (uintptr_t)&light->Ambient;
        gfx->GfxSpMovememF3d(index, offset, (void*)(data + (hasOffset == 1 ? 0x8 : 0)));
    }
    return false;
}

bool gfx_push_shader(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    const char* path = (const char*)gfx->SegAddr(cmd->words.w1);

    if (!gfx_check_image_signature(path)) {
        SPDLOG_ERROR("G_PUSH_SHADER: Shader is not a valid OTR resource name, unable to register push shader");
        return false;
    }

    path = &path[7];

    size_t shaderId = static_cast<size_t>(-1);
    for (const auto& shader : gfx->mShaders) {
        if (strcmp(shader.second, path) == 0) {
            shaderId = shader.first;
            break;
        }
    }

    if (shaderId == static_cast<size_t>(-1)) {
        shaderId = gfx->mShadersIndex++;
        gfx->mShaders[shaderId] = path;
    }

    gfx->mShaderStack.push(shaderId);

    return false;
}

bool gfx_pop_shader(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->mShaderStack.pop();

    return false;
}

const char* gfx_get_shader(int16_t id) {
    Interpreter* gfx = sGfxInstanceRaw;

    for (const std::pair<size_t, const char*>& shader : gfx->mShaders) {
        if (shader.first == id) {
            return shader.second;
        }
    }

    return nullptr; // Use no shader
}

bool gfx_moveword_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpMovewordF3dex2(C0(16, 8), C0(0, 16), cmd->words.w1);

    return false;
}

bool gfx_moveword_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpMovewordF3d(C0(0, 8), C0(8, 16), cmd->words.w1);

    return false;
}

bool gfx_texture_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTexture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));

    return false;
}

// Seems to be the same for all other non F3DEX2 microcodes...
bool gfx_texture_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTexture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));

    return false;
}

// Almost all versions of the microcode have their own version of this opcode
bool gfx_vtx_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpVertex(C0(12, 8), C0(1, 7) - C0(12, 8), (const F3DVtx*)gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_vtx_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    gfx->GfxSpVertex(C0(10, 6), C0(17, 7), (const F3DVtx*)gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_vtx_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpVertex((C0(0, 16)) / sizeof(F3DVtx), C0(16, 4), (const F3DVtx*)gfx->SegAddr(cmd->words.w1));

    return false;
}

bool gfx_vtx_hash_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    // Offset added to the start of the vertices
    const uintptr_t offset = (*cmd0)->words.w1;
    // This is a two-part display list command, so increment the instruction pointer so we can get the CRC64
    // hash from the second
    (*cmd0)++;
    const uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (*cmd0)->words.w1;

    // We need to know if the offset is a cached pointer or not. An offset greater than one million is not a
    // real offset, so it must be a real pointer
    if (offset > 0xFFFFF) {
        (*cmd0)--;
        F3DGfx* cmd = *cmd0;
        gfx->GfxSpVertex(C0(12, 8), C0(1, 7) - C0(12, 8), (F3DVtx*)offset);
        (*cmd0)++;
    } else {
        F3DVtx* vtx = (F3DVtx*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(hash);

        if (vtx != NULL) {
            vtx = (F3DVtx*)((char*)vtx + offset);

            (*cmd0)--;
            F3DGfx* cmd = *cmd0;

            // TODO: WTF??
            cmd->words.w1 = (uintptr_t)vtx;

            gfx->GfxSpVertex(C0(12, 8), C0(1, 7) - C0(12, 8), vtx);
            (*cmd0)++;
        }
    }
    return false;
}

bool gfx_vtx_otr_filepath_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    char* fileName = (char*)cmd->words.w1;
    (*cmd0)++;
    cmd = *cmd0;
    size_t vtxCnt = cmd->words.w0;
    size_t vtxIdxOff = cmd->words.w1 >> 16;
    size_t vtxDataOff = cmd->words.w1 & 0xFFFF;
    F3DVtx* vtx =
        (F3DVtx*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer((const char*)fileName);
    vtx += vtxDataOff;

    gfx->GfxSpVertex(vtxCnt, vtxIdxOff, vtx);
    return false;
}

bool gfx_dl_otr_filepath_handler_custom(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    char* fileName = (char*)cmd->words.w1;
    F3DGfx* nDL =
        (F3DGfx*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer((const char*)fileName);

    if (C0(16, 1) == 0 && nDL != nullptr) {
        g_exec_stack.call(*cmd0, nDL);
    } else {
        if (nDL != nullptr) {
            (*cmd0) = nDL;
            g_exec_stack.branch(cmd);
            return true; // shortcut cmd increment
        } else {
            assert(0 && "???");
            // gfx_path.pop_back();
            // cmd = cmd_stack.top();
            // cmd_stack.pop();
        }
    }
    return false;
}

// The original F3D microcode doesn't seem to have this opcode. Glide handles it as part of moveword
bool gfx_modify_vtx_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    gfx->GfxSpModifyVertex(C0(1, 15), C0(16, 8), (uint32_t)cmd->words.w1);
    return false;
}

// F3D, F3DEX, and F3DEX2 do the same thing but F3DEX2 has its own opcode number
#ifdef __3DS__
// [native] machines (port/3ds/gfx/gdx3ds_mesh.h): a registered machine body list reaches
// G_DL as a mesh stub; the hook draws it natively from the live RSP/RDP state and the
// stub is skipped. Weak: the interpreter links without the mesh module.
extern "C" int gdx3ds_mesh_dl_hook(void* interpreter, void* subGfx) __attribute__((weak));
#endif

bool gfx_dl_handler_common(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    F3DGfx* subGFX = (F3DGfx*)gfx->SegAddr(cmd->words.w1);
#ifdef __3DS__
    if (&gdx3ds_mesh_dl_hook != nullptr && gdx3ds_mesh_dl_hook(gfx, subGFX) != 0) {
        sGfxS7DirtyMask = 0xFFu; // the draw bound material state directly: memos re-derive
        return false;
    }
#endif
    if (C0(16, 1) == 0) {
        // Push return address
        if (subGFX != nullptr) {
            g_exec_stack.call(*cmd0, subGFX);
        }
    } else {
        (*cmd0) = subGFX;
        g_exec_stack.branch(cmd);
        return true; // shortcut cmd increment
    }
    return false;
}

bool gfx_dl_otr_hash_handler_custom(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    if (C0(16, 1) == 0) {
        // Push return address
        (*cmd0)++;

        uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (*cmd0)->words.w1;

        F3DGfx* gfx = (F3DGfx*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(hash);

        if (gfx != 0) {
            g_exec_stack.call(cmd, gfx);
        }
    } else {
        Interpreter* gfx = sGfxInstanceRaw;
        assert(0 && "????");
        (*cmd0) = (F3DGfx*)gfx->SegAddr((*cmd0)->words.w1);
        return true;
    }
    return false;
}
bool gfx_dl_index_handler(F3DGfx** cmd0) {
    // Compute seg addr by converting an index value to a offset value
    // handling 32 vs 64 bit size differences for Gfx
    // adding 1 to trigger the segaddr flow
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = (*cmd0);
    uint8_t segNum = (uint8_t)(cmd->words.w1 >> 24);
    uint32_t index = (uint32_t)(cmd->words.w1 & 0x00FFFFFF);
    uintptr_t segAddr = (segNum << 24) | (index * sizeof(F3DGfx)) + 1;

    F3DGfx* subGFX = (F3DGfx*)gfx->SegAddr(segAddr);
    if (C0(16, 1) == 0) {
        // Push return address
        if (subGFX != nullptr) {
            g_exec_stack.call((*cmd0), subGFX);
        }
    } else {
        (*cmd0) = subGFX;
        g_exec_stack.branch(cmd);
        return true; // shortcut cmd increment
    }
    return false;
}

// TODO handle special OTR opcodes later...
bool gfx_pushcd_handler_custom(F3DGfx** cmd0) {
    gfx_push_current_dir((char*)(*cmd0)->words.w1);
    return false;
}

// TODO handle special OTR opcodes later...
bool gfx_branch_z_otr_handler_f3dex2(F3DGfx** cmd0) {
    // Push return address
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = (*cmd0);

    uint8_t vbidx = (uint8_t)((*cmd0)->words.w0 & 0x00000FFF);
    uint32_t zval = (uint32_t)((*cmd0)->words.w1);

    (*cmd0)++;

    if (gfx->mRsp->loaded_vertices[vbidx].z <= zval ||
        (gfx->mRsp->extra_geometry_mode & G_EX_ALWAYS_EXECUTE_BRANCH) != 0) {
        uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (*cmd0)->words.w1;

        F3DGfx* gfx = (F3DGfx*)Ship::Context::GetInstance()->GetResourceManager()->GetResourceRawPointer(hash);

        if (gfx != 0) {
            (*cmd0) = gfx;
            g_exec_stack.branch(cmd);
            return true; // shortcut cmd increment
        }
    }
    return false;
}

bool gfx_rdphalf_1_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    gfx->mRsp->branch_z_target = (*cmd0)->words.w1;
    return false;
}

bool gfx_branch_z_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* caller = *cmd0;
    const uint8_t vbidx = static_cast<uint8_t>((caller->words.w0 & 0xFFFu) >> 1);
    const uint32_t zval = static_cast<uint32_t>(caller->words.w1);
    F3DGfx* target = reinterpret_cast<F3DGfx*>(gfx->mRsp->branch_z_target);
    gfx->mRsp->branch_z_target = 0;
    if (target == nullptr || vbidx >= MAX_VERTICES) {
        return false;
    }
    const LoadedVertex& vertex = gfx->mRsp->loaded_vertices[vbidx];
    const float screenZ =
        (fabsf(vertex.w) > 0.000001f)
            ? ((vertex.z / vertex.w) * gfx->mRsp->viewport_z_scale + gfx->mRsp->viewport_z_trans)
            : gfx->mRsp->viewport_z_trans;
    const uint32_t screenZFixed =
        static_cast<uint32_t>(std::clamp(screenZ, 0.0f, 65535.0f) * 65536.0f);

    if (screenZFixed <= zval ||
        (gfx->mRsp->extra_geometry_mode & G_EX_ALWAYS_EXECUTE_BRANCH) != 0) {
        *cmd0 = target;
        g_exec_stack.branch(caller);
        return true;
    }
    return false;
}

// F3D, F3DEX, and F3DEX2 do the same thing but F3DEX2 has its own opcode number
bool gfx_end_dl_handler_common(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    gfx->mMarkerOn = false;
    g_exec_stack.ret();
    return true;
}

bool gfx_set_prim_depth_handler_rdp(F3DGfx** cmd) {
    Interpreter* gfx = sGfxInstanceRaw;
    uint32_t w1 = (*cmd)->words.w1;
    const uint16_t newPrimDepth = (uint16_t)((w1 >> 16) & 0x7FFF); // Mask to 15 bits
    // [prim-depth-flush] Flush() samples mRdp->prim_depth lazily at drain time, so buffered
    // geometry must drain before a new value overwrites the register, or it flushes with state it
    // was never built with (the racer.c rival-icon bug). Detecting the change in GfxSpTri1 is too
    // late; the register is already overwritten. GDX_NO_PRIM_DEPTH_FLUSH=1 suppresses the drain.
    static const bool sNoPrimDepthFlush = [] {
        const char* e = std::getenv("GDX_NO_PRIM_DEPTH_FLUSH");
        return e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
    }();
    if (!sNoPrimDepthFlush && newPrimDepth != gfx->mRdp->prim_depth) {
        gfx->Flush();
    }
    gfx->mRdp->prim_depth = newPrimDepth;
    return false;
}

// Only on F3DEX2
bool gfx_geometry_mode_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode(~C0(0, 24), (uint32_t)cmd->words.w1);
    return false;
}

// Only on F3DEX and older
bool gfx_set_geometry_mode_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode(0, (uint32_t)cmd->words.w1);
    return false;
}

// Only on F3DEX and older
bool gfx_clear_geometry_mode_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode((uint32_t)cmd->words.w1, 0);
    return false;
}

bool gfx_tri1_otr_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;

    F3DGfx* cmd = *cmd0;
    uint8_t v00 = (uint8_t)(cmd->words.w0 & 0x0000FFFF);
    uint8_t v01 = (uint8_t)(cmd->words.w1 >> 16);
    uint8_t v02 = (uint8_t)(cmd->words.w1 & 0x0000FFFF);
    gfx->GfxSpTri1(v00, v01, v02, false);

    return false;
}

bool gfx_tri1_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2, false);

    return false;
}

bool gfx_tri1_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C1(17, 7), C1(9, 7), C1(1, 7), false);

    return false;
}

bool gfx_tri1_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10, false);

    return false;
}

// F3DEX, and F3DEX2 share a tri2 function, however F3DEX has a different quad function.
bool gfx_tri2_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C0(17, 7), C0(9, 7), C0(1, 7), false);
    gfx->GfxSpTri1(C1(17, 7), C1(9, 7), C1(1, 7), false);
    return false;
}

bool gfx_quad_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2, false);
    gfx->GfxSpTri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2, false);
    return false;
}

bool gfx_quad_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpTri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2, false);
    gfx->GfxSpTri1(C1(16, 8) / 2, C1(0, 8) / 2, C1(24, 8) / 2, false);
    return false;
}

bool gfx_othermode_l_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpSetOtherMode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);

    return false;
}

bool gfx_othermode_l_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpSetOtherMode(C0(8, 8), C0(0, 8), cmd->words.w1);

    return false;
}

bool gfx_othermode_h_handler_f3dex2(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpSetOtherMode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t)cmd->words.w1 << 32);

    return false;
}

// Only on F3DEX and older
bool gfx_set_geometry_mode_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode(0, (uint32_t)cmd->words.w1);
    return false;
}

// Only on F3DEX and older
bool gfx_clear_geometry_mode_handler_f3dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpGeometryMode((uint32_t)cmd->words.w1, 0);
    return false;
}

bool gfx_othermode_h_handler_f3d(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxSpSetOtherMode(C0(8, 8) + 32, C0(0, 8), (uint64_t)cmd->words.w1 << 32);

    return false;
}

bool gfx_set_timg_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    uintptr_t i = (uintptr_t)gfx->SegAddr(cmd->words.w1);

    char* imgData = (char*)i;
    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetdata = {};
    // Default scale factors to 1 for raw N64 textures. OTR textures set these
    // from the resource, but raw textures would leave them at 0.
    rawTexMetdata.h_byte_scale = 1;
    rawTexMetdata.v_pixel_scale = 1;

    if ((i & 1) != 1) {
        if (gfx_check_image_signature(imgData) == 1) {
            std::shared_ptr<Fast::Texture> tex = std::static_pointer_cast<Fast::Texture>(
                Ship::Context::GetInstance()->GetResourceManager()->LoadResourceProcess(imgData));

            if (tex == nullptr) {
                (*cmd0)++;
                return false;
            }

            i = (uintptr_t) reinterpret_cast<char*>(tex->ImageData);
            texFlags = tex->Flags;
            rawTexMetdata.width = tex->Width;
            rawTexMetdata.height = tex->Height;
            rawTexMetdata.h_byte_scale = tex->HByteScale;
            rawTexMetdata.v_pixel_scale = tex->VPixelScale;
            rawTexMetdata.type = tex->Type;
            rawTexMetdata.resource = tex;
        }
    }

    // If the resolved address is still in the N64 segmented range, SegAddr
    // failed to resolve it (segment not set up). Skip to avoid dereferencing
    // invalid memory.
    // For Windows, also check if the address is not from a dll because this validation returns a false positive caused
    // by how the virtual memory is allocated.
#ifdef _WIN32
    HMODULE module = nullptr;
    if (i <= 0x0FFFFFFF &&
        !(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(i), &module))) {
        return false;
    }
#elif defined(__3DS__)
    // 3DS userland maps the process image from 0x00100000 and the malloc heap at
    // 0x08000000 -- BOTH inside the range this guard assumes is unmapped N64
    // segmented space. The desktop check silently dropped EVERY game texture
    // (SETTIMG skipped -> no ImportTexture/UploadTexture ever ran -> all textured
    // draws sampled a stale texture id), the M1 black-top-screen root cause.
    // Only reject pointers below the process image base, which no valid host
    // allocation can occupy.
    if (i < 0x00100000) {
        return false;
    }
#else
    if (i <= 0x0FFFFFFF) {
        return false;
    }
#endif

    gfx->GfxDpSetTextureImage(C0(21, 3), C0(19, 2), C0(0, 12) + 1, imgData, texFlags, rawTexMetdata, (void*)i);

    return false;
}

bool gfx_set_timg_otr_hash_handler_custom(F3DGfx** cmd0) {
    uintptr_t addr = (*cmd0)->words.w1;
    (*cmd0)++;
    uint64_t hash = ((uint64_t)(*cmd0)->words.w0 << 32) + (uint64_t)(*cmd0)->words.w1;

    const char* fileName = Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToCString(hash);
    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetadata = {};

    if (fileName == nullptr) {
        (*cmd0)++;
        return false;
    }

    std::shared_ptr<Fast::Texture> texture =
        std::static_pointer_cast<Fast::Texture>(Ship::Context::GetInstance()->GetResourceManager()->LoadResourceProcess(
            Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToCString(hash)));
    if (texture != nullptr) {
        texFlags = texture->Flags;
        rawTexMetadata.width = texture->Width;
        rawTexMetadata.height = texture->Height;
        rawTexMetadata.h_byte_scale = texture->HByteScale;
        rawTexMetadata.v_pixel_scale = texture->VPixelScale;
        rawTexMetadata.type = texture->Type;
        rawTexMetadata.resource = texture;

        // OTRTODO: We have disabled caching for now to fix a texture corruption issue with HD texture
        // support. In doing so, there is a potential performance hit since we are not caching lookups. We
        // need to do proper profiling to see whether or not it is worth it to keep the caching system.

        char* tex = reinterpret_cast<char*>(texture->ImageData);

        if (tex != nullptr) {
            (*cmd0)--;
            uintptr_t oldData = (*cmd0)->words.w1;
            // TODO: wtf??
            (*cmd0)->words.w1 = (uintptr_t)tex;

            // if (ourHash != (uint64_t)-1) {
            //     auto res = ResourceLoad(ourHash);
            // }

            (*cmd0)++;
        }

        (*cmd0)--;
        F3DGfx* cmd = (*cmd0);
        uint32_t fmt = C0(21, 3);
        uint32_t size = C0(19, 2);
        uint32_t width = C0(0, 12) + 1;

        if (tex != NULL) {
            Interpreter* gfx = sGfxInstanceRaw;
            gfx->GfxDpSetTextureImage(fmt, size, width, fileName, texFlags, rawTexMetadata, tex);
        }
    } else {
        SPDLOG_ERROR("G_SETTIMG_OTR_HASH: Texture is null");
    }

    (*cmd0)++;
    return false;
}

#ifdef __3DS__
// --------------------------------------------------------------------------------
// SELECT-PERF: per-SETTIMG resource-resolution memo.
//
// F-Zero X's o2r pipeline (port/n64_gfx_bridge.cpp) rewrites asset-segment SETTIMGs
// to OTR_G_SETTIMG_OTR_FILEPATH commands whose w1 is an interned key string, so the
// handler below ran ResourceManager::LoadResourceProcess for EVERY textured resource
// draw, EVERY frame: ResourceIdentifier construction (std::string copy), the OTR
// signature/alt-path checks, the cache mutex, two string-keyed map lookups and the
// LRU tick — ~93 times/frame on the machine-select screens (30-machine grid + decals
// + stat bars + fonts), a real slice of the 23-34 ms menu build on the 268 MHz ARM11.
//
// The memo is a direct-mapped table keyed on the SETTIMG identity itself (the w1 key
// string POINTER — the bridge interns these, so the pointer is stable per texture).
// Safety is layered, none of it trusting the pointer alone:
//   - the payload is a weak_ptr<Fast::Texture>: resource-cache eviction (the 3DS
//     24 MiB budget, lus-resource-cache-cap.patch) drops the cache's shared_ptr and
//     the weak_ptr expires by construction — no generation counter, no stale entry
//     window, and the memo never pins a resource's memory (weak refs do not count
//     toward the evictor's use_count() > 1 pin test);
//   - a hit verifies the resolved texture's InitData Path against the key string
//     (with the "__OTR__" prefix skipped, matching LoadResourceProcess), so a
//     recycled/repointed key string can never bind another resource's texture — the
//     same identity discipline as the stable-key patch, at strcmp cost;
//   - every kGdxSettimgMemoLease-th hit falls through to the full LoadResourceProcess
//     anyway, refreshing the resource-cache LRU tick so the byte-budget evictor still
//     sees these entries as hot (a memo that never touched the cache would age its
//     own textures to the front of the eviction queue).
// A miss (expired, mismatched, collided or leased-out) runs the pre-existing path
// bit-for-bit and re-arms the slot. Alt-assets loads (path rewritten to alt/...)
// fail the Path check and simply never memo — correct, just not accelerated.
// --------------------------------------------------------------------------------
namespace {

// Mirrors ImportTexture's stable-resource-key derivation (the resource-stable-key
// patch): 64-bit FNV-1a over the archive path, xor-folded to 32 bits, 0 promoted to 1
// (the unhashed-key sentinel). MUST stay value-identical to that code so memoized and
// non-memoized loads of the same resource mint the same texture-cache key.
uint32_t GdxFoldResourcePathHash(const std::shared_ptr<Fast::Texture>& tex) {
    const auto initData = tex != nullptr ? tex->GetInitData() : nullptr;
    uint64_t pathHash = 0;
    if (initData != nullptr) {
        pathHash = 1469598103934665603ull;
        for (const char c : initData->Path) {
            pathHash = (pathHash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        }
    }
    uint32_t folded = static_cast<uint32_t>(pathHash ^ (pathHash >> 32));
    if (folded == 0) {
        folded = 1;
    }
    return folded;
}

struct GdxSettimgMemoEntry {
    const char* key = nullptr; // the SETTIMG w1 string pointer (bridge-interned)
    std::weak_ptr<Fast::Texture> tex;
    uint32_t pathHash = 0; // GdxFoldResourcePathHash of the resolved texture
    uint32_t hitsSinceLease = 0;
};

constexpr size_t kGdxSettimgMemoSlots = 512; // power of two; direct-mapped, replace on collision
constexpr uint32_t kGdxSettimgMemoLease = 256; // full lookup every N hits: keeps the cache LRU warm
GdxSettimgMemoEntry sGdxSettimgMemo[kGdxSettimgMemoSlots];

GdxSettimgMemoEntry& GdxSettimgMemoSlot(const char* key) {
    // Fibonacci scramble of the pointer; low bits are alignment-dead, so index from
    // the product's high half.
    const uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key)) * 2654435761u;
    return sGdxSettimgMemo[(h >> 20) & (kGdxSettimgMemoSlots - 1)];
}

std::shared_ptr<Fast::Texture> GdxSettimgMemoLookup(const char* fileName, uint32_t* outPathHash) {
    GdxSettimgMemoEntry& entry = GdxSettimgMemoSlot(fileName);
    if (entry.key != fileName) {
        return nullptr;
    }
    if (++entry.hitsSinceLease >= kGdxSettimgMemoLease) {
        entry.hitsSinceLease = 0;
        return nullptr; // lease expired: take the full path once to tick the cache LRU
    }
    std::shared_ptr<Fast::Texture> tex = entry.tex.lock();
    if (tex == nullptr) {
        entry.key = nullptr; // resource evicted/unloaded; re-resolve and re-arm
        return nullptr;
    }
    const char* path = fileName;
    if (strncmp(path, "__OTR__", 7) == 0) {
        path += 7; // LoadResourceProcess strips the signature before caching
    }
    const auto initData = tex->GetInitData();
    if (initData == nullptr || strcmp(initData->Path.c_str(), path) != 0) {
        entry.key = nullptr; // key pointer no longer names this resource
        return nullptr;
    }
    *outPathHash = entry.pathHash;
    return tex;
}

void GdxSettimgMemoStore(const char* fileName, const std::shared_ptr<Fast::Texture>& tex, uint32_t pathHash) {
    GdxSettimgMemoEntry& entry = GdxSettimgMemoSlot(fileName);
    entry.key = fileName;
    entry.tex = tex;
    entry.pathHash = pathHash;
    entry.hitsSinceLease = 0;
}

} // namespace
#endif // __3DS__

bool gfx_set_timg_otr_filepath_handler_custom(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    const char* fileName = (char*)cmd->words.w1;

    uint32_t texFlags = 0;
    RawTexMetadata rawTexMetadata = {};

#ifdef __3DS__
    uint32_t memoPathHash = 0;
    std::shared_ptr<Fast::Texture> texture = GdxSettimgMemoLookup(fileName, &memoPathHash);
    if (texture != nullptr) {
        gdx3ds_settimg_note_memo();
    } else {
        gdx3ds_settimg_note_load();
        texture = std::static_pointer_cast<Fast::Texture>(
            Ship::Context::GetInstance()->GetResourceManager()->LoadResourceProcess(fileName));
        if (texture != nullptr) {
            memoPathHash = GdxFoldResourcePathHash(texture);
            GdxSettimgMemoStore(fileName, texture, memoPathHash);
        }
    }
#else
    std::shared_ptr<Fast::Texture> texture = std::static_pointer_cast<Fast::Texture>(
        Ship::Context::GetInstance()->GetResourceManager()->LoadResourceProcess(fileName));
#endif
    if (texture != nullptr) {
        Interpreter* gfx = sGfxInstanceRaw;
        texFlags = texture->Flags;
        rawTexMetadata.width = texture->Width;
        rawTexMetadata.height = texture->Height;
        rawTexMetadata.h_byte_scale = texture->HByteScale;
        rawTexMetadata.v_pixel_scale = texture->VPixelScale;
        rawTexMetadata.type = texture->Type;
        rawTexMetadata.resource = texture;
#ifdef __3DS__
        // Precomputed stable resource key (memo hit or the store above): ImportTexture
        // skips its per-import path FNV when this is nonzero.
        rawTexMetadata.path_hash = memoPathHash;
#endif

        uint32_t fmt = C0(21, 3);
        uint32_t size = C0(19, 2);
        uint32_t width = C0(0, 12) + 1;

        gfx->GfxDpSetTextureImage(fmt, size, width, fileName, texFlags, rawTexMetadata,
                                  reinterpret_cast<char*>(texture->ImageData));
    } else {
        static uint32_t sMissingTextureLogs = 0;
        if (sMissingTextureLogs < 8) {
            ++sMissingTextureLogs;
            SPDLOG_ERROR("G_SETTIMG_OTR_FILEPATH: Texture '{}' is null", fileName);
            if (sMissingTextureLogs == 8) {
                SPDLOG_ERROR("G_SETTIMG_OTR_FILEPATH: further missing-texture errors suppressed");
            }
        }
    }
    return false;
}

bool gfx_set_fb_handler_custom(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    Interpreter* gfx = sGfxInstanceRaw;
    gfx->Flush();

    if (cmd->words.w1) {
        gfx->SetFrameBuffer((int32_t)cmd->words.w1, 1.0f);
        gfx->mActiveFrameBuffer = gfx->mFrameBuffers.find((int32_t)cmd->words.w1);
        gfx->mFbActive = true;
    } else {
        gfx->ResetFrameBuffer();
        gfx->mFbActive = false;
        gfx->mActiveFrameBuffer = gfx->mFrameBuffers.end();
    }
    return false;
}

bool gfx_reset_fb_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    gfx->Flush();
    gfx->mFbActive = false;
    gfx->mActiveFrameBuffer = gfx->mFrameBuffers.end();
    gfx->mRapi->StartDrawToFramebuffer(gfx->mRendersToFb ? gfx->mGameFb : 0,
                                       (float)gfx->mCurDimensions.height / gfx->mNativeDimensions.height);
    // Force viewport and scissor to reapply against the main framebuffer, in case a previous smaller
    // framebuffer truncated the values
    gfx->mRdp->viewport_or_scissor_changed = true;
    gfx->mRenderingState.viewport = {};
    gfx->mRenderingState.scissor = {};
    return false;
}

bool gfx_copy_fb_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    bool* hasCopiedPtr = (bool*)cmd->words.w1;

    gfx->Flush();
    gfx->CopyFrameBuffer(C0(11, 11), C0(0, 11), (bool)C0(22, 1), hasCopiedPtr);
    return false;
}

bool gfx_read_fb_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    int32_t width, height;
    [[maybe_unused]] int32_t ulx, uly;
    uint16_t* rgba16Buffer = (uint16_t*)cmd->words.w1;
    int fbId = C0(0, 8);
    bool bswap = C0(8, 1);
    ++(*cmd0);
    cmd = *cmd0;
    // Specifying the upper left origin value is unused and unsupported at the renderer level
    ulx = C0(0, 16);
    uly = C0(16, 16);
    width = C1(0, 16);
    height = C1(16, 16);

    gfx->Flush();
    gfx->mRapi->ReadFramebufferToCPU(fbId, width, height, rgba16Buffer);

#ifndef IS_BIGENDIAN
    // byteswap the output to BE
    if (bswap) {
        for (size_t i = 0; i < (size_t)width * height; i++) {
            rgba16Buffer[i] = BE16SWAP(rgba16Buffer[i]);
        }
    }
#endif

    return false;
}

bool gfx_register_blended_texture_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    // Flush incase we are replacing a previous blended texture that hasn't been finialized to the GPU
    gfx->Flush();

    char* timg = (char*)cmd->words.w1;

    ++(*cmd0);
    cmd = *cmd0;

    uint8_t* mask = (uint8_t*)cmd->words.w0;
    uint8_t* replacementTex = (uint8_t*)cmd->words.w1;

    if (!gfx_check_image_signature(timg)) {
        SPDLOG_ERROR(
            "OTR_G_REGBLENDEDTEX: Texture is not a valid OTR resource name, unable to register blended texture");
        return false;
    }

    // With no mask, we should clear the blended texture
    if (mask == nullptr) {
        gfx->UnregisterBlendedTexture(timg);
    } else {
        gfx->RegisterBlendedTexture(timg, mask, replacementTex);
    }

    return false;
}

bool gfx_set_timg_fb_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->Flush();
    gfx->mRapi->SelectTextureFb((uint32_t)cmd->words.w1);
    gfx->mRdp->textures_changed[0] = false;
    gfx->mRdp->textures_changed[1] = false;
    return false;
}

bool gfx_set_grayscale_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->mRdp->grayscale = cmd->words.w1;
    return false;
}

bool gfx_load_block_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpLoadBlock(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
    return false;
}

bool gfx_load_block_wide_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    uint32_t tile = cmd->words.w0 & 0x7;
    uint32_t lrs = cmd->words.w1;

    (*cmd0)++;
    cmd = *cmd0;

    uint32_t uls = (cmd->words.w0 >> 16) & 0xFFFF;
    uint32_t ult = (cmd->words.w0 >> 0) & 0xFFFF;
    uint32_t dxt = (cmd->words.w1 >> 0) & 0xFFF;

    gfx->GfxDpLoadBlock(tile, uls, ult, lrs, dxt);
    return false;
}

bool gfx_load_tile_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpLoadTile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
    return false;
}

bool gfx_set_tile_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetTile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4),
                      C1(8, 2), C1(4, 4), C1(0, 4));
    return false;
}

bool gfx_set_tile_size_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetTileSize(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
    return false;
}

bool gfx_set_tile_size_interp_handler_rdp(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    Interpreter* gfx = sGfxInstanceRaw;

    if (gfx->mInterpolationIndex == gfx->mInterpolationIndexTarget) {
        int tile = C1(24, 3);
        gfx->GfxDpSetTileSize(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
        ++(*cmd0);
        memcpy(&gfx->mRdp->texture_tile[tile].uls, &(*cmd0)->words.w0, sizeof(float));
        memcpy(&gfx->mRdp->texture_tile[tile].ult, &(*cmd0)->words.w1, sizeof(float));
        ++(*cmd0);
        memcpy(&gfx->mRdp->texture_tile[tile].lrs, &(*cmd0)->words.w0, sizeof(float));
        memcpy(&gfx->mRdp->texture_tile[tile].lrt, &(*cmd0)->words.w1, sizeof(float));
    } else {
        ++(*cmd0);
        ++(*cmd0);
    }

    return false;
}

bool gfx_set_interpolation_index_target(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    Interpreter* gfx = sGfxInstanceRaw;

    gfx->mInterpolationIndexTarget = cmd->words.w1;
    return false;
}

bool gfx_load_tlut_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpLoadTlut(C1(24, 3), C1(14, 10));
    return false;
}

bool gfx_set_env_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetEnvColor(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

bool gfx_set_prim_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetPrimColor(C0(8, 8), C0(0, 8), C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

bool gfx_set_fog_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetFogColor(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

// CENTER/SCALE and K4/K5 are wired as combiner inputs, so the standard (A-B)*C+D
// shader path covers their common uses. TODO: chroma-key width/threshold
// gating from G_SETKEYR/GB (wR/wG/wB ignored) and the YUV->RGB matrix K0..K3
// applied during texture sampling.
// G_SETKEYR: w1 = [wR:12 | cR:8 | sR:8]
bool gfx_set_key_r_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->mRdp->key_center.r = C1(8, 8);
    gfx->mRdp->key_scale.r = C1(0, 8);
    return false;
}

// G_SETKEYGB: w0 = [op:8 | wG:12 | _:4 | wB:12], w1 = [cG:8 | sG:8 | cB:8 | sB:8]
bool gfx_set_key_gb_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->mRdp->key_center.g = C1(24, 8);
    gfx->mRdp->key_scale.g = C1(16, 8);
    gfx->mRdp->key_center.b = C1(8, 8);
    gfx->mRdp->key_scale.b = C1(0, 8);
    return false;
}

// G_SETCONVERT: w0 = [op:8 | k0:9 | k1:9 | k2_hi:4], w1 = [k2_lo:5 | k3:9 | k4:9 | k5:9]
// K0..K5 are signed 9-bit values; sign-extend after decoding.
bool gfx_set_convert_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->mRdp->convert_k[0] = sign_extend_9(C0(13, 9));
    gfx->mRdp->convert_k[1] = sign_extend_9(C0(4, 9));
    // k2 is split across w0 and w1
    gfx->mRdp->convert_k[2] = sign_extend_9((C0(0, 4) << 5) | C1(27, 5));
    gfx->mRdp->convert_k[3] = sign_extend_9(C1(18, 9));
    gfx->mRdp->convert_k[4] = sign_extend_9(C1(9, 9));
    gfx->mRdp->convert_k[5] = sign_extend_9(C1(0, 9));
    return false;
}

bool gfx_set_blend_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    // [blend-alpha-flush] Same hazard as [prim-depth-flush]: Flush() samples blend_color.a lazily
    // for the alpha-compare threshold, so buffered geometry must drain before the register
    // changes. Only alpha matters; r/g/b never reach Flush(). GDX_NO_BLEND_ALPHA_FLUSH=1
    // suppresses the drain.
    const uint8_t newBlendAlpha = (uint8_t)C1(0, 8);
    static const bool sNoBlendAlphaFlush = [] {
        const char* e = std::getenv("GDX_NO_BLEND_ALPHA_FLUSH");
        return e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
    }();
    if (!sNoBlendAlphaFlush && newBlendAlpha != gfx->mRdp->blend_color.a) {
        gfx->Flush();
    }
    gfx->GfxDpSetBlendColor(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

bool gfx_set_fill_color_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetFillColor((uint32_t)cmd->words.w1);
    return false;
}

bool gfx_set_intensity_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetGrayscaleColor(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
    return false;
}

bool gfx_set_combine_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;

    gfx->GfxDpSetCombineMode(
        color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)), alpha_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)),
        color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)), alpha_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));
    return false;
}

bool gfx_tex_rect_and_flip_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    int8_t opcode = (int8_t)(cmd->words.w0 >> 24);
    int32_t lrx, lry, tile, ulx, uly;
    uint32_t uls, ult, dsdx, dtdy;

    lrx = C0(12, 12);
    lry = C0(0, 12);
    tile = C1(24, 3);
    ulx = C1(12, 12);
    uly = C1(0, 12);
    // TODO make sure I don't need to increment cmd0
    ++(*cmd0);
    cmd = *cmd0;
    uls = C1(16, 16);
    ult = C1(0, 16);
    ++(*cmd0);
    cmd = *cmd0;
    dsdx = C1(16, 16);
    dtdy = C1(0, 16);

    gfx->GfxDpTextureRectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == RDP_G_TEXRECTFLIP);
    return false;
}

bool gfx_tex_rect_wide_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    int8_t opcode = (int8_t)(cmd->words.w0 >> 24);
    int32_t lrx, lry, tile, ulx, uly;
    uint32_t uls, ult, dsdx, dtdy;

    lrx = static_cast<int32_t>((C0(0, 24) << 8)) >> 8;
    lry = static_cast<int32_t>((C1(0, 24) << 8)) >> 8;
    tile = C1(24, 3);
    ++(*cmd0);
    cmd = *cmd0;
    ulx = static_cast<int32_t>((C0(0, 24) << 8)) >> 8;
    uly = static_cast<int32_t>((C1(0, 24) << 8)) >> 8;
    ++(*cmd0);
    cmd = *cmd0;
    uls = C0(16, 16);
    ult = C0(0, 16);
    dsdx = C1(16, 16);
    dtdy = C1(0, 16);
    gfx->GfxDpTextureRectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == RDP_G_TEXRECTFLIP);
    return false;
}

bool gfx_image_rect_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *cmd0;
    int16_t tile, iw, ih;
    int16_t x0, y0, s0, t0;
    int16_t x1, y1, s1, t1;
    tile = C0(0, 3);
    iw = C1(16, 16);
    ih = C1(0, 16);
    cmd = ++(*cmd0);
    x0 = C0(16, 16);
    y0 = C0(0, 16);
    s0 = C1(16, 16);
    t0 = C1(0, 16);
    cmd = ++(*cmd0);
    x1 = C0(16, 16);
    y1 = C0(0, 16);
    s1 = C1(16, 16);
    t1 = C1(0, 16);
    gfx->GfxDpImageRectangle(tile, iw, ih, x0, y0, s0, t0, x1, y1, s1, t1);

    return false;
}

bool gfx_fill_rect_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpFillRectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
    return false;
}

bool gfx_fill_wide_rect_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);
    int32_t lrx, lry, ulx, uly;

    lrx = (int32_t)(C0(0, 24) << 8) >> 8;
    lry = (int32_t)(C1(0, 24) << 8) >> 8;
    cmd = ++(*cmd0);
    ulx = (int32_t)(C0(0, 24) << 8) >> 8;
    uly = (int32_t)(C1(0, 24) << 8) >> 8;
    gfx->GfxDpFillRectangle(ulx, uly, lrx, lry);

    return false;
}

bool gfx_SetScissor_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpSetScissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
    return false;
}

bool gfx_set_z_img_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpSetZImage(gfx->SegAddr(cmd->words.w1));
    return false;
}

bool gfx_set_c_img_handler_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpSetColorImage(C0(21, 3), C0(19, 2), C0(0, 11), gfx->SegAddr(cmd->words.w1));
    return false;
}

bool gfx_rdp_set_other_mode_rdp(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    gfx->GfxDpSetOtherMode(C0(0, 24), (uint32_t)cmd->words.w1);
    return false;
}

bool gfx_bg_copy_handler_s2dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    if (!gfx->mMarkerOn) {
        gfx->Gfxs2dexBgCopy((F3DuObjBg*)cmd->words.w1); // not gfx->SegAddr here it seems
    }
    return false;
}

bool gfx_bg_1cyc_handler_s2dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    gfx->Gfxs2dexBg1cyc((F3DuObjBg*)cmd->words.w1);
    return false;
}

bool gfx_obj_rectangle_handler_s2dex(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    if (!gfx->mMarkerOn) {
        gfx->Gfxs2dexRecyCopy((F3DuObjSprite*)cmd->words.w1); // not gfx->SegAddr here it seems
    }
    return false;
}

bool gfx_extra_geometry_mode_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    gfx->GfxSpExtraGeometryMode(~C0(0, 24), (uint32_t)cmd->words.w1);
    return false;
}

// L3DEX2 G_LINE3D rewritten by the port's gfx bridge. Operand encoding is preserved: vertex
// indices arrive pre-multiplied by 2, exactly like native L3DEX2.
bool gfx_line3d_gdx_handler_custom(F3DGfx** cmd0) {
    Interpreter* gfx = sGfxInstanceRaw;
    F3DGfx* cmd = *(cmd0);

    const uint8_t v0 = (uint8_t)(C0(16, 8) / 2);
    const uint8_t v1 = (uint8_t)(C0(8, 8) / 2);
    const uint8_t wd = (uint8_t)C0(0, 8);
    gfx->GfxSpLine3DGdx(v0, v1, wd);
    return false;
}

bool gfx_stubbed_command_handler(F3DGfx** cmd0) {
    return false;
}

// gDPPipeSync: hardware drains its pipeline here so later state changes cannot retroactively
// affect queued draws. The drain was once removed as redundant with the source-side
// [prim-depth-flush]/[blend-alpha-flush] guards, but removing it coincided with the Mute City sky
// alternating every other tick. GDX_NO_PIPESYNC_FLUSH=1 restores the no-drain behaviour.
//
// [traffic] 3DS: the drain is now OFF by default. This renderer orders draws by submission, so
// the only hazards a pipesync ever guarded here are states sampled AFTER append time, and every
// one of those now flushes at its own mutation site when the value actually changes:
//   - prim_depth      -> gfx_set_prim_depth_handler_rdp   ([prim-depth-flush], value-change Flush)
//   - blend_color.a   -> gfx_set_blend_color_handler_rdp  ([blend-alpha-flush], value-change Flush)
//   - prim/env color  -> GfxDpSetPrimColor/GfxDpSetEnvColor (lus-3ds-primenv-flush, value-change
//                        Flush; the 3DS backend binds these as batch constants from vertex 0)
//   - fog line        -> the G_MW_FOG moveword handlers (lus-3ds-fog-exact-params, value-change
//                        Flush; the 3DS backend binds ONE fog LUT per draw)
//   - fog color       -> GfxDpSetFogColor (value-change Flush added WITH this change: the fog
//                        blend stage constant is also hoisted from vertex 0, and game DLs pair
//                        gDPSetFogColor with a pipesync — the drain was masking that one hazard)
// Everything else (combiner/shader, othermode-derived depth/decal/alpha, viewport/scissor,
// samplers, texture imports) flushes inside GfxSpTri1's state application when the state
// differs from mRenderingState — a pipesync between them is pure overhead. The Mute City
// sky-alternation that once justified the drain predates the primenv/fog value-change guards,
// which cover exactly that defect class. Traffic frames carry ~450 pipesyncs; each drain that
// found a non-empty batch split it into a tiny extra draw (~208 draws/frame, drw/dsp overhead).
// GDX_PIPESYNC_FLUSH=1 restores the unconditional drain for A/B.
bool gfx_rdp_pipe_sync_handler_rdp(F3DGfx** cmd0) {
#ifdef __3DS__
    static const bool sForceFlush = [] {
        const char* e = std::getenv("GDX_PIPESYNC_FLUSH");
        return e != nullptr && e[0] != 0 && !(e[0] == '0' && e[1] == 0);
    }();
    if (sForceFlush) {
        Interpreter* gfx = sGfxInstanceRaw;
        gfx->Flush();
    }
#else
    static const bool sNoFlush = [] {
        const char* e = std::getenv("GDX_NO_PIPESYNC_FLUSH");
        return e != nullptr && e[0] != 0 && !(e[0] == '0' && e[1] == 0);
    }();
    if (!sNoFlush) {
        Interpreter* gfx = sGfxInstanceRaw;
        gfx->Flush();
    }
#endif
    return false;
}

bool gfx_spnoop_command_handler_f3dex2(F3DGfx** cmd0) {
    return false;
}

class UcodeHandler {
  public:
    inline constexpr UcodeHandler(
        std::initializer_list<std::pair<int8_t, std::pair<const char*, GfxOpcodeHandlerFunc>>> initializer) {
        std::fill(std::begin(mHandlers), std::end(mHandlers),
                  std::pair<const char*, GfxOpcodeHandlerFunc>(nullptr, nullptr));

        for (const auto& [opcode, handler] : initializer) {
            mHandlers[static_cast<uint8_t>(opcode)] = handler;
        }
    }

    inline bool contains(int8_t opcode) const {
        return mHandlers[static_cast<uint8_t>(opcode)].first != nullptr;
    }

    inline std::pair<const char*, GfxOpcodeHandlerFunc> at(int8_t opcode) const {
        return mHandlers[static_cast<uint8_t>(opcode)];
    }

  private:
    std::pair<const char*, GfxOpcodeHandlerFunc> mHandlers[std::numeric_limits<uint8_t>::max() + 1];
};

static constexpr UcodeHandler rdpHandlers = {
    { RDP_G_SETTARGETINTERPINDEX,
      { "G_SETTARGETINTERPINDEX", gfx_set_interpolation_index_target } }, // G_SETTARGETINTERPINDEX
    { RDP_G_SETTILESIZE_INTERP,
      { "G_SETTILESIZE_INTERP", gfx_set_tile_size_interp_handler_rdp } },            // G_SETTILESIZE_INTERP
    { RDP_G_TEXRECT, { "G_TEXRECT", gfx_tex_rect_and_flip_handler_rdp } },           // G_TEXRECT (-28)
    { RDP_G_TEXRECTFLIP, { "G_TEXRECTFLIP", gfx_tex_rect_and_flip_handler_rdp } },   // G_TEXRECTFLIP (-27)
    { RDP_G_RDPLOADSYNC, { "mRdpLOADSYNC", gfx_stubbed_command_handler } },          // mRdpLOADSYNC (-26)
    // Real Flush(), not a stub: the no-op let state changes made after a sync
    // retroactively affect draws queued before it (see gfx_rdp_pipe_sync_handler_rdp).
    { RDP_G_RDPPIPESYNC, { "mRdpPIPESYNC", gfx_rdp_pipe_sync_handler_rdp } },        // mRdpPIPESYNC (-25)
    { RDP_G_RDPTILESYNC, { "mRdpTILESYNC", gfx_stubbed_command_handler } },          // mRdpPIPESYNC (-24)
    { RDP_G_RDPFULLSYNC, { "mRdpFULLSYNC", gfx_stubbed_command_handler } },          // mRdpFULLSYNC (-23)
    { RDP_G_SETKEYGB, { "G_SETKEYGB", gfx_set_key_gb_handler_rdp } },                // G_SETKEYGB (-22)
    { RDP_G_SETKEYR, { "G_SETKEYR", gfx_set_key_r_handler_rdp } },                   // G_SETKEYR (-21)
    { RDP_G_SETCONVERT, { "G_SETCONVERT", gfx_set_convert_handler_rdp } },           // G_SETCONVERT (-20)
    { RDP_G_SETSCISSOR, { "G_SETSCISSOR", gfx_SetScissor_handler_rdp } },            // G_SETSCISSOR (-19)
    { RDP_G_SETPRIMDEPTH, { "G_SETPRIMDEPTH", gfx_set_prim_depth_handler_rdp } },    // G_SETPRIMDEPTH (-18)
    { RDP_G_RDPSETOTHERMODE, { "mRdpSETOTHERMODE", gfx_rdp_set_other_mode_rdp } },   // mRdpSETOTHERMODE (-17)
    { RDP_G_LOADTLUT, { "G_LOADTLUT", gfx_load_tlut_handler_rdp } },                 // G_LOADTLUT (-16)
    { RDP_G_SETTILESIZE, { "G_SETTILESIZE", gfx_set_tile_size_handler_rdp } },       // G_SETTILESIZE (-14)
    { RDP_G_LOADBLOCK, { "G_LOADBLOCK", gfx_load_block_handler_rdp } },              // G_LOADBLOCK (-13)
    { RDP_G_LOADTILE, { "G_LOADTILE", gfx_load_tile_handler_rdp } },                 // G_LOADTILE (-12)
    { RDP_G_SETTILE, { "G_SETTILE", gfx_set_tile_handler_rdp } },                    // G_SETTILE (-11)
    { RDP_G_FILLRECT, { "G_FILLRECT", gfx_fill_rect_handler_rdp } },                 // G_FILLRECT (-10)
    { RDP_G_SETFILLCOLOR, { "G_SETFILLCOLOR", gfx_set_fill_color_handler_rdp } },    // G_SETFILLCOLOR (-9)
    { RDP_G_SETFOGCOLOR, { "G_SETFOGCOLOR", gfx_set_fog_color_handler_rdp } },       // G_SETFOGCOLOR (-8)
    { RDP_G_SETBLENDCOLOR, { "G_SETBLENDCOLOR", gfx_set_blend_color_handler_rdp } }, // G_SETBLENDCOLOR (-7)
    { RDP_G_SETPRIMCOLOR, { "G_SETPRIMCOLOR", gfx_set_prim_color_handler_rdp } },    // G_SETPRIMCOLOR (-6)
    { RDP_G_SETENVCOLOR, { "G_SETENVCOLOR", gfx_set_env_color_handler_rdp } },       // G_SETENVCOLOR (-5)
    { RDP_G_SETCOMBINE, { "G_SETCOMBINE", gfx_set_combine_handler_rdp } },           // G_SETCOMBINE (-4)
    { RDP_G_SETTIMG, { "G_SETTIMG", gfx_set_timg_handler_rdp } },                    // G_SETTIMG (-3)
    { RDP_G_SETZIMG, { "G_SETZIMG", gfx_set_z_img_handler_rdp } },                   // G_SETZIMG (-2)
    { RDP_G_SETCIMG, { "G_SETCIMG", gfx_set_c_img_handler_rdp } },                   // G_SETCIMG (-1)
};

static constexpr UcodeHandler otrHandlers = {
    { OTR_G_SETTIMG_OTR_HASH,
      { "G_SETTIMG_OTR_HASH", gfx_set_timg_otr_hash_handler_custom } },       // G_SETTIMG_OTR_HASH (0x20)
    { OTR_G_SETFB, { "G_SETFB", gfx_set_fb_handler_custom } },                // G_SETFB (0x21)
    { OTR_G_RESETFB, { "G_RESETFB", gfx_reset_fb_handler_custom } },          // G_RESETFB (0x22)
    { OTR_G_SETTIMG_FB, { "G_SETTIMG_FB", gfx_set_timg_fb_handler_custom } }, // G_SETTIMG_FB (0x23)
    { OTR_G_VTX_OTR_FILEPATH,
      { "G_VTX_OTR_FILEPATH", gfx_vtx_otr_filepath_handler_custom } }, // G_VTX_OTR_FILEPATH (0x24)
    { OTR_G_SETTIMG_OTR_FILEPATH,
      { "G_SETTIMG_OTR_FILEPATH", gfx_set_timg_otr_filepath_handler_custom } }, // G_SETTIMG_OTR_FILEPATH (0x25)
    { OTR_G_TRI1_OTR, { "G_TRI1_OTR", gfx_tri1_otr_handler_f3dex2 } },          // G_TRI1_OTR (0x26)
    { OTR_G_DL_OTR_FILEPATH, { "G_DL_OTR_FILEPATH", gfx_dl_otr_filepath_handler_custom } }, // G_DL_OTR_FILEPATH (0x27)
    { OTR_G_PUSHCD, { "G_PUSHCD", gfx_pushcd_handler_custom } },                            // G_PUSHCD (0x28)
    { OTR_G_MTX_OTR_FILEPATH,
      { "G_MTX_OTR_FILEPATH", gfx_mtx_otr_filepath_handler_custom } },          // G_MTX_OTR_FILEPATH (0x29)
    { OTR_G_DL_OTR_HASH, { "G_DL_OTR_HASH", gfx_dl_otr_hash_handler_custom } }, // G_DL_OTR_HASH (0x31)
    { OTR_G_VTX_OTR_HASH, { "G_VTX_OTR_HASH", gfx_vtx_hash_handler_custom } },  // G_VTX_OTR_HASH (0x32)
    { OTR_G_MARKER, { "G_MARKER", gfx_marker_handler_otr } },                   // G_MARKER (0X33)
    { OTR_G_INVALTEXCACHE, { "G_INVALTEXCACHE", gfx_invalidate_tex_cache_handler_f3dex2 } }, // G_INVALTEXCACHE (0X34)
    { OTR_G_BRANCH_Z_OTR, { "G_BRANCH_Z_OTR", gfx_branch_z_otr_handler_f3dex2 } },           // G_BRANCH_Z_OTR (0x35)
    { OTR_G_MTX_OTR, { "G_MTX_OTR", gfx_mtx_otr_handler_custom } },                          // G_MTX_OTR (0x36)
    { OTR_G_TEXRECT_WIDE, { "G_TEXRECT_WIDE", gfx_tex_rect_wide_handler_custom } },          // G_TEXRECT_WIDE (0x37)
    { OTR_G_FILLWIDERECT, { "G_FILLWIDERECT", gfx_fill_wide_rect_handler_custom } },         // G_FILLWIDERECT (0x38)
    { OTR_G_SETGRAYSCALE, { "G_SETGRAYSCALE", gfx_set_grayscale_handler_custom } },          // G_SETGRAYSCALE (0x39)
    { OTR_G_EXTRAGEOMETRYMODE,
      { "G_EXTRAGEOMETRYMODE", gfx_extra_geometry_mode_handler_custom } }, // G_EXTRAGEOMETRYMODE (0x3a)
    { OTR_G_COPYFB, { "G_COPYFB", gfx_copy_fb_handler_custom } },          // G_COPYFB (0x3b)
    { OTR_G_IMAGERECT, { "G_IMAGERECT", gfx_image_rect_handler_custom } }, // G_IMAGERECT (0x3c)
    { OTR_G_DL_INDEX, { "G_DL_INDEX", gfx_dl_index_handler } },            // G_DL_INDEX (0x3d)
    { OTR_G_READFB, { "G_READFB", gfx_read_fb_handler_custom } },          // G_READFB (0x3e)
    { OTR_G_REGBLENDEDTEX,
      { "G_REGBLENDEDTEX", gfx_register_blended_texture_handler_custom } },         // G_REGBLENDEDTEX (0x3f)
    { OTR_G_SETINTENSITY, { "G_SETINTENSITY", gfx_set_intensity_handler_custom } }, // G_SETINTENSITY (0x40)
    { OTR_G_LINE3D_GDX, { "G_LINE3D_GDX", gfx_line3d_gdx_handler_custom } },        // G_LINE3D_GDX (0x41)
    { OTR_G_MOVEMEM_HASH, { "OTR_G_MOVEMEM_HASH", gfx_movemem_handler_otr } },      // OTR_G_MOVEMEM_HASH
    { OTR_G_PUSH_SHADER, { "G_PUSH_SHADER", gfx_push_shader } },
    { OTR_G_POP_SHADER, { "G_POP_SHADER", gfx_pop_shader } },
    { RDP_G_LOADBLOCK_WIDE, { "G_LOADBLOCK_WIDE", gfx_load_block_wide_handler_rdp } }, // RDP_G_LOADBLOCK_WIDE (-15)
    { RDP_G_VTX_WIDE, { "G_VTX_WIDE", gfx_vtx_handler_f3dex2 } },                      // RDP_G_VTX_WIDE (-16)
    { RDP_G_TRI1_WIDE, { "G_TRI1_WIDE", gfx_tri1_handler_f3dex2 } },                   // RDP_G_TRI1_WIDE (-17)
};

static constexpr UcodeHandler f3dex2Handlers = {
    { F3DEX2_G_NOOP, { "G_NOOP", gfx_noop_handler_f3dex2 } },
    { F3DEX2_G_SPNOOP, { "G_SPNOOP", gfx_noop_handler_f3dex2 } },
    { F3DEX2_G_CULLDL, { "G_CULLDL", gfx_cull_dl_handler_f3dex2 } },
    { F3DEX2_G_MTX, { "G_MTX", gfx_mtx_handler_f3dex2 } },
    { F3DEX2_G_POPMTX, { "G_POPMTX", gfx_pop_mtx_handler_f3dex2 } },
    { F3DEX2_G_MOVEMEM, { "G_MOVEMEM", gfx_movemem_handler_f3dex2 } },
    { F3DEX2_G_DMA_IO, { "G_DMA_IO", gfx_dma_io_handler_f3dex2 } },
    { F3DEX2_G_MOVEWORD, { "G_MOVEWORD", gfx_moveword_handler_f3dex2 } },
    { F3DEX2_G_TEXTURE, { "G_TEXTURE", gfx_texture_handler_f3dex2 } },
    { F3DEX2_G_VTX, { "G_VTX", gfx_vtx_handler_f3dex2 } },
    { F3DEX2_G_MODIFYVTX, { "G_MODIFYVTX", gfx_modify_vtx_handler_f3dex2 } },
    { F3DEX2_G_RDPHALF_1, { "G_RDPHALF_1", gfx_rdphalf_1_handler_f3dex2 } },
    { F3DEX2_G_BRANCH_Z, { "G_BRANCH_Z", gfx_branch_z_handler_f3dex2 } },
    { F3DEX2_G_DL, { "G_DL", gfx_dl_handler_common } },
    { F3DEX2_G_ENDDL, { "G_ENDDL", gfx_end_dl_handler_common } },
    { F3DEX2_G_GEOMETRYMODE, { "G_GEOMETRYMODE", gfx_geometry_mode_handler_f3dex2 } },
    { F3DEX2_G_TRI1, { "G_TRI1", gfx_tri1_handler_f3dex2 } },
    { F3DEX2_G_TRI2, { "G_TRI2", gfx_tri2_handler_f3dex } },
    { F3DEX2_G_QUAD, { "G_QUAD", gfx_quad_handler_f3dex2 } },
    { F3DEX2_G_SETOTHERMODE_L, { "G_SETOTHERMODE_L", gfx_othermode_l_handler_f3dex2 } },
    { F3DEX2_G_SETOTHERMODE_H, { "G_SETOTHERMODE_H", gfx_othermode_h_handler_f3dex2 } },
};

static constexpr UcodeHandler f3dexHandlers = {
    { F3DEX_G_NOOP, { "G_NOOP", gfx_noop_handler_f3dex2 } },
    { F3DEX_G_CULLDL, { "G_CULLDL", gfx_cull_dl_handler_f3dex2 } },
    { F3DEX_G_MTX, { "G_MTX", gfx_mtx_handler_f3d } },
    { F3DEX_G_POPMTX, { "G_POPMTX", gfx_pop_mtx_handler_f3d } },
    { F3DEX_G_MOVEMEM, { "G_POPMEM", gfx_movemem_handler_f3d } },
    { F3DEX_G_MOVEWORD, { "G_MOVEWORD", gfx_moveword_handler_f3d } },
    { F3DEX_G_TEXTURE, { "G_TEXTURE", gfx_texture_handler_f3d } },
    { F3DEX_G_SETOTHERMODE_L, { "G_SETOTHERMODE_L", gfx_othermode_l_handler_f3d } },
    { F3DEX_G_SETOTHERMODE_H, { "G_SETOTHERMODE_H", gfx_othermode_h_handler_f3d } },
    { F3DEX_G_SETGEOMETRYMODE, { "G_SETGEOMETRYMODE", gfx_set_geometry_mode_handler_f3dex } },
    { F3DEX_G_CLEARGEOMETRYMODE, { "G_CLEARGEOMETRYMODE", gfx_clear_geometry_mode_handler_f3dex } },
    { F3DEX_G_VTX, { "G_VTX", gfx_vtx_handler_f3dex } },
    { F3DEX_G_TRI1, { "G_TRI1", gfx_tri1_handler_f3dex } },
    { F3DEX_G_MODIFYVTX, { "G_MODIFYVTX", gfx_modify_vtx_handler_f3dex2 } },
    { F3DEX_G_DL, { "G_DL", gfx_dl_handler_common } },
    { F3DEX_G_ENDDL, { "G_ENDDL", gfx_end_dl_handler_common } },
    { F3DEX_G_TRI2, { "G_TRI2", gfx_tri2_handler_f3dex } },
    { F3DEX_G_SPNOOP, { "G_SPNOOP", gfx_spnoop_command_handler_f3dex2 } },
    { F3DEX_G_RDPHALF_1, { "mRdpHALF_1", gfx_stubbed_command_handler } },
    { F3DEX_G_QUAD, { "G_QUAD", gfx_quad_handler_f3dex } },
};

static constexpr UcodeHandler f3dHandlers = {
    { F3DEX_G_NOOP, { "G_NOOP", gfx_noop_handler_f3dex2 } },
    { F3DEX_G_CULLDL, { "G_CULLDL", gfx_cull_dl_handler_f3dex2 } },
    { F3DEX_G_MTX, { "G_MTX", gfx_mtx_handler_f3d } },
    { F3DEX_G_POPMTX, { "G_POPMTX", gfx_pop_mtx_handler_f3d } },
    { F3DEX_G_MOVEMEM, { "G_POPMEM", gfx_movemem_handler_f3d } },
    { F3DEX_G_MOVEWORD, { "G_MOVEWORD", gfx_moveword_handler_f3d } },
    { F3DEX_G_TEXTURE, { "G_TEXTURE", gfx_texture_handler_f3d } },
    { F3DEX_G_SETOTHERMODE_L, { "G_SETOTHERMODE_L", gfx_othermode_l_handler_f3d } },
    { F3DEX_G_SETOTHERMODE_H, { "G_SETOTHERMODE_H", gfx_othermode_h_handler_f3d } },
    { F3DEX_G_SETGEOMETRYMODE, { "G_SETGEOMETRYMODE", gfx_set_geometry_mode_handler_f3dex } },
    { F3DEX_G_CLEARGEOMETRYMODE, { "G_CLEARGEOMETRYMODE", gfx_clear_geometry_mode_handler_f3dex } },
    { F3DEX_G_VTX, { "G_VTX", gfx_vtx_handler_f3d } },
    { F3DEX_G_TRI1, { "G_TRI1", gfx_tri1_handler_f3d } },
    { F3DEX_G_MODIFYVTX, { "G_MODIFYVTX", gfx_modify_vtx_handler_f3dex2 } },
    { F3DEX_G_DL, { "G_DL", gfx_dl_handler_common } },
    { F3DEX_G_ENDDL, { "G_ENDDL", gfx_end_dl_handler_common } },
    { F3DEX_G_TRI2, { "G_TRI2", gfx_tri2_handler_f3dex } },
    { F3DEX_G_SPNOOP, { "G_SPNOOP", gfx_spnoop_command_handler_f3dex2 } },
    { F3DEX_G_RDPHALF_1, { "mRdpHALF_1", gfx_stubbed_command_handler } },
};

// LUSTODO: These S2DEX commands have different opcode numbers on F3DEX2 vs other ucodes. More research needs to be done
// to see if the implementations are different.
static constexpr UcodeHandler s2dexHandlers = {
    { F3DEX2_G_BG_COPY, { "G_BG_COPY", gfx_bg_copy_handler_s2dex } },
    { F3DEX2_G_BG_1CYC, { "G_BG_1CYC", gfx_bg_1cyc_handler_s2dex } },
    { F3DEX2_G_OBJ_RENDERMODE, { "G_OBJ_RENDERMODE", gfx_stubbed_command_handler } },
    { F3DEX2_G_OBJ_RECTANGLE_R, { "G_OBJ_RECTANGLE_R", gfx_stubbed_command_handler } },
    { F3DEX2_G_OBJ_RECTANGLE, { "G_OBJ_RECTANGLE", gfx_obj_rectangle_handler_s2dex } },
    { F3DEX2_G_DL, { "G_DL", gfx_dl_handler_common } },
    { F3DEX2_G_ENDDL, { "G_ENDDL", gfx_end_dl_handler_common } },
};

static constexpr std::array ucode_handlers = {
    &f3dHandlers,    // ucode_f3db
    &f3dHandlers,    // ucode_f3d
    &f3dexHandlers,  // ucode_f3dex
    &f3dexHandlers,  // ucode_f3dexb
    &f3dex2Handlers, // ucode_f3dex2
    &s2dexHandlers,  // ucode_s2dex
};

const char* GfxGetOpcodeName(int8_t opcode) {
    if (otrHandlers.contains(opcode)) {
        return otrHandlers.at(opcode).first;
    }

    if (rdpHandlers.contains(opcode)) {
        return rdpHandlers.at(opcode).first;
    }

    if (ucode_handler_index < ucode_handlers.size()) {
        if (ucode_handlers[ucode_handler_index]->contains(opcode)) {
            return ucode_handlers[ucode_handler_index]->at(opcode).first;
        } else {
            SPDLOG_CRITICAL("Unhandled OP code: 0x{:X}, for loaded ucode: {}", (uint8_t)opcode,
                            (uint32_t)ucode_handler_index);
        }
    } else {
        SPDLOG_CRITICAL("Unhandled OP code: 0x{:X}, invalid ucode: {}", (uint8_t)opcode, (uint32_t)ucode_handler_index);
    }

    return nullptr;
}

// [perf-dispatch] Rebuilds the flat merged dispatch table below whenever the selected ucode
// changes. Defined after the handler tables it merges; forward-declared for the writers here.
static void gfx_rebuild_flat_dispatch();

// TODO, implement a system where we can get the current opcode handler by writing to the GWords. If the powers that be
// are OK with that...
static void gfx_set_ucode_handler(UcodeHandlers ucode) {
    // Loaded ucode must be in range of the supported ucode_handlers
    assert(ucode < ucode_max);
    Interpreter* gfx = sGfxInstanceRaw;
    const bool handlerChanged = (ucode_handler_index != ucode);
    ucode_handler_index = ucode;

    // Same handler table means an in-family variant switch (F-Zero X issues G_LOAD_UCODE from
    // F3DEX2 to F3DFLX2.Rej mid-frame, before every machine). Hardware does not clear RSP
    // constants on such a load, and the game relies on Course_Draw's fog position staying live
    // for the car draws that follow, so apply the quirk resets only on a real table change.
    if (!handlerChanged) {
        // Same ucode_handler_index value -> the flat dispatch merge is unchanged too.
        return;
    }

    gfx_rebuild_flat_dispatch();

    // Reset some RSP state values upon ucode load to deal with hardware quirks discovered by emulators
    switch (ucode) {
        case ucode_f3d:
        case ucode_f3db:
        case ucode_f3dex:
        case ucode_f3dexb:
        case ucode_f3dex2:
            gfx->mRsp->fog_mul = 0;
            gfx->mRsp->fog_offset = 0;
#ifdef __3DS__
            gdx3ds_fog_note_params(0, 0, 1);
#endif
            break;
        default:
            break;
    }
}

// [perf-s7] True for opcodes that can never change per-draw state (combiner, othermodes, tile
// descriptors, textures, viewport/scissor, colors): pure geometry and control flow. Everything
// else conservatively invalidates the tri draw-state memo. F3DEX2-family values only — the
// caller additionally requires ucode_handler_index == ucode_f3dex2, so colliding F3D/S2DEX
// encodings (e.g. F3D G_MOVEMEM == 0x03) can never slip through the whitelist.
static inline bool GfxS7OpcodePreservesDrawState(int8_t opcode) {
    switch (opcode) {
        case F3DEX2_G_NOOP:          // OPEN/CLOSE_DISPS bookkeeping only
        case F3DEX2_G_VTX:
        case F3DEX2_G_MODIFYVTX:
        case F3DEX2_G_CULLDL:
        case F3DEX2_G_BRANCH_Z:
        case F3DEX2_G_TRI1:
        case F3DEX2_G_TRI2:
        case F3DEX2_G_QUAD:
        case F3DEX2_G_MTX:           // vertex-transform state only
        case F3DEX2_G_POPMTX:
        case F3DEX2_G_DL:
        case F3DEX2_G_ENDDL:
        case OTR_G_VTX_OTR_FILEPATH:
        case OTR_G_TRI1_OTR:
        case OTR_G_DL_OTR_FILEPATH:
        case OTR_G_MTX_OTR_FILEPATH:
        case OTR_G_DL_OTR_HASH:
        case OTR_G_VTX_OTR_HASH:
        case OTR_G_MARKER:
        case OTR_G_BRANCH_Z_OTR:
        case OTR_G_MTX_OTR:
        case OTR_G_DL_INDEX:
        case RDP_G_VTX_WIDE:
        case RDP_G_TRI1_WIDE:
        // [traffic] RDP sync ops: their handlers mutate NO interpreter state (the loads/tile/full
        // syncs are stubs; the pipesync drain is a batch split at most — it never touches the
        // tile descriptors, loaded slots, othermode or sampler state the memo derives from).
        // F-Zero X race DLs carry ~450 G_RDPPIPESYNCs per traffic frame interleaved with the
        // machine-part draws; before this row each one armed the dirty mask and forced the next
        // G_TRI2 into a FULL state re-resolution (extent divides + ApplyTileMaskExtent + two
        // virtual backend calls) — the [profop] 06=16us/call traffic thrash.
        case RDP_G_RDPPIPESYNC:
        case RDP_G_RDPLOADSYNC:
        case RDP_G_RDPTILESYNC:
        case RDP_G_RDPFULLSYNC:
        // [traffic] Draw-constant colour registers: nothing the memo banks reads prim/env colour
        // or the combine word. The batch-constant hazard is handled at the mutation site
        // (GfxDpSetPrimColor/GfxDpSetEnvColor flush on a VALUE change, lus-3ds-primenv-flush),
        // and combiner identity is a memo KEY (s7Memo.comb / s7Memo.prg compare), so a combine
        // change that resolves to a different ColorCombiner misses the memo by construction and
        // re-banks through the slow path. A re-assertion of the SAME combine (the dominant
        // machine-part pattern: 30 machines sharing a handful of materials) now keeps the memo.
        case RDP_G_SETPRIMCOLOR:
        case RDP_G_SETENVCOLOR:
        case RDP_G_SETCOMBINE:
        // [crowd2] Tile-descriptor and load-path opcodes moved to CONDITIONAL dirtying at
        // their mutation sites: the handlers OR the legacy kGfxS7Dirty* contribution into
        // sGfxS7DirtyMask (and arm textures_changed) exactly when they change state —
        // a value-identical SETTILE/SETTILESIZE re-assertion and a same-content
        // LOADBLOCK/LOADTILE replay leave no observable state and preserve the memos.
        // SETTIMG only stages texture_to_load (consumed by the loaders, never by the
        // draw-state derivation), so it is unconditionally preserve. The FB variant
        // (OTR_G_SETTIMG_FB) binds framebuffer textures and stays OFF the whitelist.
        case RDP_G_SETTILE:
        case RDP_G_SETTILESIZE:
        case RDP_G_LOADBLOCK:
        case RDP_G_LOADBLOCK_WIDE:
        case RDP_G_LOADTILE:
        case RDP_G_SETTIMG:
        case OTR_G_SETTIMG_OTR_HASH:
        case OTR_G_SETTIMG_OTR_FILEPATH:
            return true;
        default:
            return false;
    }
}

// [texrect] Rect-run memo whitelist: opcodes that can never invalidate the RECT-path draw
// state (sGfxS7RectMemo). Strictly the tri whitelist MINUS the triangle-emitting ops — their
// GfxSpTri1 applies GAME sampler/shader state to mRenderingState, which the rect fast path
// must be able to prove untouched since banking — PLUS the texrect opcodes themselves, whose
// in-handler mutations are each covered: first_tile_index is a memo key, textures_changed is
// probed live, and the COPY-mode combiner/textfilt overrides are pure functions of the
// surrounding othermode state (unchanged while the whitelist holds). G_FILLRECT and
// OTR_G_IMAGE_RECT are deliberately NOT whitelisted (fill mutates combine/fill state around
// its draw; image-rect rewrites tile descriptors and TMEM slot fields); their draws re-bank
// the rect memo through the slow path, which keeps them exact.
static inline bool GfxS7OpcodePreservesRectState(int8_t opcode) {
    switch (opcode) {
        case F3DEX2_G_TRI1:
        case F3DEX2_G_TRI2:
        case F3DEX2_G_QUAD:
        case OTR_G_TRI1_OTR:
        case RDP_G_TRI1_WIDE:
            return false;
        case RDP_G_TEXRECT:
        case RDP_G_TEXRECTFLIP:
        case OTR_G_TEXRECT_WIDE:
            return true;
        default:
            return GfxS7OpcodePreservesDrawState(opcode);
    }
}

// [perf-dispatch] Flat merged dispatch table. The legacy gfx_step chain probed up to three
// handler tables per command (otrHandlers -> rdpHandlers -> ucode_handlers[ucode]), each probe
// a contains() + an at() returning a {name, fn} pair by value, plus a per-command
// GfxS7OpcodePreservesDrawState switch, an OTR-filepath opcode scan and a G_LOAD_UCODE compare.
// The precedence of that chain is STATIC once the ucode is selected, so it is merged here into
// one 256-slot array of {fn, flags} at ucode-selection time (init, gfx_set_ucode_handler,
// Interpreter ucode reset, gfx_set_target_ucode) and gfx_step becomes a single indexed load and
// one indirect call. Slots the chain would have missed hold the matching diagnostic handler so
// the unknown-op behaviour (SPDLOG_CRITICAL + advance) is byte-for-byte the old one.
struct GfxFlatDispatchEntry {
    GfxOpcodeHandlerFunc fn;
    uint8_t flags;
    // [texrect] Precomputed sGfxS7DirtyMask contribution (kGfxS7Dirty* bits) of this opcode:
    // gfx_step ORs it in unconditionally, replacing the legacy PreservesDrawState branch.
    uint8_t dirty;
};

enum : uint8_t {
    // Set iff (ucode_handler_index == ucode_f3dex2 && GfxS7OpcodePreservesDrawState(op)) held at
    // table-build time: precomputes the [perf-s7] tri draw-state memo whitelist per slot.
    kGfxFlatFlagPreservesDrawState = 1u << 0,
    // OTR filepath handlers expect w1 to be a valid string pointer; these slots run the
    // null/N64-segment-address guard before calling the handler (legacy: an opcode scan).
    kGfxFlatFlagOtrPointerGuard = 1u << 1,
};

static GfxFlatDispatchEntry sFlatDispatch[std::numeric_limits<uint8_t>::max() + 1];

// G_LOAD_UCODE preceded every table probe in the legacy chain; the flat table gives its slot
// this handler unconditionally. Returning false falls through to gfx_step's ++cmd, which is
// exactly the legacy `++cmd; return;`.
static bool gfx_load_ucode_flat_handler(F3DGfx** cmd0) {
    F3DGfx* cmd = *cmd0;
    const uintptr_t payload = static_cast<uintptr_t>(cmd->words.w1);
    if ((payload & ~static_cast<uintptr_t>(0xFFu)) == F3DEX2_VARIANT_SWITCH_MARKER) {
        const uint8_t rawVariant = static_cast<uint8_t>(payload & 0xFFu);
        if (rawVariant <= static_cast<uint8_t>(F3dex2Variant::FZeroFlxReject)) {
            gfx_set_ucode_handler(ucode_f3dex2);
            sGfxInstanceRaw->SetF3dex2Variant(static_cast<F3dex2Variant>(rawVariant));
        }
    } else {
        gfx_set_ucode_handler((UcodeHandlers)(cmd->words.w0 & 0xFFFFFF));
    }
    return false;
}

static bool gfx_unhandled_opcode_flat_handler(F3DGfx** cmd0) {
    const int8_t opcode = (int8_t)((*cmd0)->words.w0 >> 24);
    SPDLOG_CRITICAL("Unhandled OP code: 0x{:X}, for loaded ucode: {}", (uint8_t)opcode,
                    (uint32_t)ucode_handler_index);
    return false;
}

static bool gfx_invalid_ucode_flat_handler(F3DGfx** cmd0) {
    const int8_t opcode = (int8_t)((*cmd0)->words.w0 >> 24);
    SPDLOG_CRITICAL("Unhandled OP code: 0x{:X}, invalid ucode: {}", (uint8_t)opcode, (uint32_t)ucode_handler_index);
    return false;
}

static void gfx_rebuild_flat_dispatch() {
    const bool ucodeValid = ucode_handler_index < ucode_handlers.size();
    const UcodeHandler* ucodeTable = ucodeValid ? ucode_handlers[ucode_handler_index] : nullptr;

    for (int i = 0; i <= (int)std::numeric_limits<uint8_t>::max(); i++) {
        const int8_t opcode = (int8_t)(uint8_t)i;
        GfxFlatDispatchEntry entry{ nullptr, 0, 0 };

        // Same precedence as the legacy chain: otr overrides > rdp > selected ucode table.
        if (otrHandlers.contains(opcode)) {
            entry.fn = otrHandlers.at(opcode).second;
            if (opcode == OTR_G_VTX_OTR_FILEPATH || opcode == OTR_G_SETTIMG_OTR_FILEPATH ||
                opcode == OTR_G_DL_OTR_FILEPATH || opcode == OTR_G_PUSHCD || opcode == OTR_G_MTX_OTR_FILEPATH) {
                entry.flags |= kGfxFlatFlagOtrPointerGuard;
            }
        } else if (rdpHandlers.contains(opcode)) {
            entry.fn = rdpHandlers.at(opcode).second;
        } else if (!ucodeValid) {
            entry.fn = gfx_invalid_ucode_flat_handler;
        } else if (ucodeTable->contains(opcode)) {
            entry.fn = ucodeTable->at(opcode).second;
        } else {
            entry.fn = gfx_unhandled_opcode_flat_handler;
        }

        // [perf-s7] tri draw-state memo whitelist, precomputed per slot (see gfx_step).
        if (ucode_handler_index == ucode_f3dex2 && GfxS7OpcodePreservesDrawState(opcode)) {
            entry.flags |= kGfxFlatFlagPreservesDrawState;
        } else {
            entry.dirty |= kGfxS7DirtyTri;
        }
        // [texrect] rect-run memo whitelist (same f3dex2 gate — collisions can't slip through).
        // The cached rect-bypass viewport rides the same whitelist: conservative (tri draws
        // cannot move its inputs) but a recompute costs once per rect run, not per rect.
        if (!(ucode_handler_index == ucode_f3dex2 && GfxS7OpcodePreservesRectState(opcode))) {
            entry.dirty |= kGfxS7DirtyRect | kGfxS7DirtyRectViewport;
        }

        sFlatDispatch[i] = entry;
    }

    // Installed last so it overrides whatever the merge put in its slot, mirroring the legacy
    // early check. Deliberately no PreservesDrawState flag and a full dirty mask: G_LOAD_UCODE
    // dirtied the memo before, and a mid-run ucode switch must dirty the rect memo too.
    sFlatDispatch[(uint8_t)(int8_t)F3DEX2_G_LOAD_UCODE] = {
        gfx_load_ucode_flat_handler, 0, (uint8_t)(kGfxS7DirtyTri | kGfxS7DirtyRect | kGfxS7DirtyRectViewport)
    };
}

// Build the initial table for the static ucode_f3dex2 default. The constexpr handler tables it
// merges are constant-initialized, so they are complete before any dynamic initializer runs.
[[maybe_unused]] static const bool sFlatDispatchInitialized = (gfx_rebuild_flat_dispatch(), true);

static void gfx_step() {
    auto& cmd = g_exec_stack.currCmd();
    int8_t opcode = (int8_t)(cmd->words.w0 >> 24);
#ifdef __3DS__
    if (gdx3ds_verify_mode != 0) { /* [nverify] diagnostic, inline-gated: see gdx3ds_verify.h */
        gdx3ds_verify_note_cmd((unsigned)cmd->words.w0, (unsigned)cmd->words.w1);
    }
#endif

    // [perf-dispatch] one indexed load; the entry is read fully before the handler runs so a
    // mid-command table rebuild (G_LOAD_UCODE) cannot pull it out from under us.
    const GfxFlatDispatchEntry entry = sFlatDispatch[(uint8_t)opcode];

    // [perf-s7]/[texrect] draw-state memo dirty tracking: per-opcode contribution precomputed
    // into entry.dirty at table build (branchless OR replaces the legacy whitelist branch).
    sGfxS7DirtyMask |= entry.dirty;

#ifdef USE_GBI_TRACE
    if (cmd->words.trace.valid &&
        Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger("gEnableGFXTrace", 0)) {
#define TRACE                                  \
    "\n====================================\n" \
    " - CMD: {:02X}\n"                         \
    " - Path: {}:{}\n"                         \
    " - W0: {:08X}\n"                          \
    " - W1: {:08X}\n"                          \
    "===================================="
        SPDLOG_INFO(TRACE, (uint8_t)opcode, cmd->words.trace.file, cmd->words.trace.idx, cmd->words.w0, cmd->words.w1);
    }
#endif

    if ((entry.flags & kGfxFlatFlagOtrPointerGuard) != 0) {
        // OTR filepath handlers expect w1 to be a valid string pointer.
        // Guard against null or N64-segment addresses that would crash in strlen/strncmp.
        uintptr_t w1 = (uintptr_t)cmd->words.w1;
        if (w1 < 0x10000
#if UINTPTR_MAX > 0xFFFFFFFFu
            // On 64-bit: filter kernel/sentinel addresses.
            || w1 > 0x0000FFFFFFFFFFFFull
#endif
        ) {
            ++g_exec_stack.currCmd();
            return;
        }
    }

#ifdef __3DS__
    // [profop] per-opcode handler timing at the single dispatch call site. Names WHICH
    // handler bodies own the 26ms "dsp" bucket. Gated on the latched gputrace flag.
    if (gdx3ds_prof_active) {
        const uint8_t opIdx = (uint8_t)opcode;
        const long long t0 = gdx3ds_prof_now();
        const bool jumped = entry.fn(&cmd);
        gdx3ds_prof_op_ticks[opIdx] += (uint64_t)(gdx3ds_prof_now() - t0);
        gdx3ds_prof_op_calls[opIdx] += 1;
        if (jumped) {
            return;
        }
        ++cmd;
        return;
    }
#endif
    if (entry.fn(&cmd)) {
        return;
    }

    ++cmd;
}

void Interpreter::SpReset() {
    GfxS7InvalidateDrawMemos(); // [perf-s7] per-task boundary
#ifdef __3DS__
    GdxTriFastRefresh(); // [trifast] per-task killswitch latch
#endif
    while (!mShaderStack.empty()) {
        mShaderStack.pop();
    }
    // A widescreen anchor/stretch scope set by a display list that was branched over or
    // aborted mid-frame must not leak into the next task: a latched G_EX_WIDESCREEN_STRETCH
    // would mis-stretch every subsequent 2D rect. The game clears its scopes explicitly on
    // every normal path; this is the per-task backstop.
    mRsp->extra_geometry_mode = 0;
    mRsp->modelview_matrix_stack_size = 1;
    mRsp->branch_z_target = 0;
    mRsp->viewport_z_scale = 511.0f;
    mRsp->viewport_z_trans = 511.0f;
    mRsp->current_num_lights = 2;
    mRsp->lights_changed = true;
    memset(mRsp->dmem, 0, sizeof(mRsp->dmem));
    mRsp->dma_io_dmem = 0;
    mRsp->dma_io_loaded = false;
    mRsp->f3dflx_alpha_light_valid = false;
    mRsp->lookat[0].dir[0] = 0;
    mRsp->lookat[0].dir[1] = 127;
    mRsp->lookat[0].dir[2] = 0;
    mRsp->lookat[1].dir[0] = 127;
    mRsp->lookat[1].dir[1] = 0;
    mRsp->lookat[1].dir[2] = 0;
    CalculateNormalDir(&mRsp->lookat[0], mRsp->current_lookat_coeffs[0]);
    CalculateNormalDir(&mRsp->lookat[1], mRsp->current_lookat_coeffs[1]);
}

void Interpreter::RegisterFbTexture(const void* cpuAddr, int fbId) {
    mFbTextures[(uintptr_t)cpuAddr] = fbId;
}

void Interpreter::UnregisterFbTexture(const void* cpuAddr) {
    mFbTextures.erase((uintptr_t)cpuAddr);
}

void Interpreter::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    mWapi->GetDimensions(width, height, posX, posY);
}

void Interpreter::Init(class GfxWindowBackend* wapi, class GfxRenderingAPI* rapi, const char* game_name,
                       bool start_in_fullscreen, uint32_t width, uint32_t height, uint32_t posX, uint32_t posY) {
    mWapi = wapi;
    mRapi = rapi;
    mWapi->Init(game_name, rapi->GetName(), start_in_fullscreen, width, height, posX, posY);
    mRapi->Init();
    mRapi->UpdateFramebufferParameters(0, width, height, 1, false, true, true, true);
    mCurDimensions.internal_mul =
        Ship::Context::GetInstance()->GetConsoleVariables()->GetFloat(CVAR_INTERNAL_RESOLUTION, 1);
    mMsaaLevel = Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_MSAA_VALUE, 1);

    mCurDimensions.width = width;
    mCurDimensions.height = height;

    mGameFb = mRapi->CreateFramebuffer();
    mGameFbMsaaResolved = mRapi->CreateFramebuffer();

    mNativeDimensions.width = SCREEN_WIDTH;
    mNativeDimensions.height = SCREEN_HEIGHT;

    for (int i = 0; i < MAX_SEGMENT_POINTERS; i++) {
        mSegmentPointers[i] = 0;
    }

    if (mTexUploadBuffer == nullptr) {
        // We cap texture max to 8k, because why would you need more?
        int max_tex_size = std::min(8192, mRapi->GetMaxTextureSize());
        // C4c: on this backend GetMaxTextureSize() is the PICA200's 1024, so the stock
        // reservation below is 4,194,304 B held for the life of the process -- rank 3 of the
        // whole heap in the allocation census and 16.7% of an Old 3DS memtype-0 grant, and
        // no [mem-res] row has ever named it. [debug] tex_upload_kb (DEFAULT 0) keeps
        // exactly this size; a non-zero value reserves that many KiB instead, and every
        // decoder above is clamped to whatever it gets. port/3ds/gdx3ds_texupload.c.
        const size_t stockBytes = (size_t)max_tex_size * (size_t)max_tex_size * 4u;
        const size_t texBytes = gdx3ds_tex_upload_pick_size(stockBytes);
        mTexUploadBuffer = (uint8_t*)malloc(texBytes);
    }

    ucode_handler_index = UcodeHandlers::ucode_f3dex2;
    gfx_rebuild_flat_dispatch(); // [perf-dispatch] table tracks every ucode_handler_index write

    // Pre-allocate texture cache buckets to prevent rehash-induced iterator invalidation.
    mTextureCache.map.reserve(TEXTURE_CACHE_MAX_SIZE);
}

void Interpreter::Destroy() {
    // TODO: should also destroy rapi, and any other resources acquired in fast3d
    free(mTexUploadBuffer);
    mWapi->Destroy();

    // Texture cache and loaded textures store references to Resources which need to be unreferenced.
    TextureCacheClear();
    mRdp->texture_to_load.raw_tex_metadata.resource = nullptr;
    for (auto& loadedTexture : mRdp->loaded_texture) {
        loadedTexture.raw_tex_metadata.resource = nullptr;
    }
}

GfxRenderingAPI* Interpreter::GetCurrentRenderingAPI() {
    return mRapi;
}

void Interpreter::SetGfxDebugger(std::shared_ptr<GfxDebugger> debugger) {
    mGfxDebugger = std::move(debugger);
}

std::shared_ptr<GfxDebugger> Interpreter::GetGfxDebugger() const {
    return mGfxDebugger;
}

void Interpreter::HandleWindowEvents() {
    mWapi->HandleEvents();
}

bool Interpreter::IsFrameReady() {
    return mWapi->IsFrameReady();
}

bool Interpreter::ViewportMatchesRendererResolution() {
#ifdef __APPLE__
    // Always treat the viewport as not matching the render resolution on mac
    // to avoid issues with retina scaling.
    return false;
#else
    if (mCurDimensions.width == mGameWindowViewport.width && mCurDimensions.height == mGameWindowViewport.height) {
        return true;
    }
    return false;
#endif
}

// Whether StartFrame sized mGameFbMsaaResolved this frame. Run() re-latches the fixed-aspect flag
// mid-frame, so a mode flip after StartFrame can request an MSAA resolve into a target StartFrame
// never allocated; the epilogue then resolves to 0 instead of publishing an unallocated buffer.
static bool sGameFbMsaaResolvedSized = false;

void Interpreter::StartFrame() {
    // [perf-s7] backend clip parameters / framebuffer bindings can change between frames
    // without an opcode in between (resize, present) — never carry the memos across frames.
    GfxS7InvalidateDrawMemos();
    mWapi->GetDimensions(&mGfxCurrentWindowDimensions.width, &mGfxCurrentWindowDimensions.height, &mCurWindowPosX,
                         &mCurWindowPosY);
    if (mCurDimensions.height == 0) {
        // Avoid division by zero
        mCurDimensions.height = 1;
    }
    mCurDimensions.aspect_ratio = (float)mCurDimensions.width / (float)mCurDimensions.height;

    // Update the framebuffer sizes when the viewport or native dimension changes
    if (mCurDimensions.width != mPrvDimensions.width || mCurDimensions.height != mPrvDimensions.height ||
        mNativeDimensions.width != mPrevNativeDimensions.width ||
        mNativeDimensions.height != mPrevNativeDimensions.height) {

        for (auto& fb : mFrameBuffers) {
            uint32_t width = fb.second.orig_width, height = fb.second.orig_height;
            if (fb.second.resize) {
                AdjustWidthHeightForScale(width, height, fb.second.native_width, fb.second.native_height);
            }
            if (width != fb.second.applied_width || height != fb.second.applied_height) {
                mRapi->UpdateFramebufferParameters(fb.first, width, height, 1, true, true, true, true);
                fb.second.applied_width = width;
                fb.second.applied_height = height;
            }
        }
    }

    mPrvDimensions = mCurDimensions;
    mPrevNativeDimensions = mNativeDimensions;
    // Latch the widescreen CVars once per frame; the per-vertex and per-rect consumers read these
    // members instead of doing CVar lookups on the hot path. Run() re-latches the fixed-aspect
    // flag per task. Widescreen == 0 pillarboxes via Fast3dGui::DrawGame, which needs the frame in
    // an offscreen texture -- hence the forced offscreen target below even at 1x with no MSAA.
    mWidescreenEnabledCache = CVarGetInteger("gEnhancements.Graphics.Widescreen", 1) != 0;
    mForceFixedAspectCache = sGdxForceFixedAspect != 0;
    mWidescreenUiCache = CVarGetInteger("gEnhancements.Graphics.WidescreenUI", 0) != 0;
    mUltrawideCache = CVarGetInteger("gEnhancements.Graphics.UltrawideMode", 0) != 0;
    mRemoveBordersCache = CVarGetInteger("gEnhancements.Graphics.RemoveBorders", 0) != 0;
    // HudMaxAspect: 0 or any out-of-range value means no clamp, so ANCHOR-scoped HUD elements
    // glue to the true screen corners. A value in [4:3, 8) confines the HUD to a centred band of
    // that aspect.
    mHudMaxAspectCache = CVarGetFloat("gEnhancements.Graphics.HudMaxAspect", 0.0f);
    if (!(mHudMaxAspectCache >= 4.0f / 3.0f && mHudMaxAspectCache < 8.0f)) {
        mHudMaxAspectCache = 1000.0f; // min(currentAspect, 1000) == currentAspect -> old edge-glue math
    }
    const bool widescreenPillarbox = !mWidescreenEnabledCache || mForceFixedAspectCache;
    sGameFbMsaaResolvedSized = false;
    if (!ViewportMatchesRendererResolution() || mMsaaLevel > 1 || widescreenPillarbox) {
        mRendersToFb = true;
        if (!ViewportMatchesRendererResolution() || (widescreenPillarbox && mMsaaLevel <= 1)) {
            mRapi->UpdateFramebufferParameters(mGameFb, mCurDimensions.width, mCurDimensions.height, mMsaaLevel, true,
                                               true, true, true);
        } else {
            // MSAA framebuffer needs to be resolved to an equally sized target when complete, which must therefore
            // match the window size
            mRapi->UpdateFramebufferParameters(mGameFb, mGfxCurrentWindowDimensions.width,
                                               mGfxCurrentWindowDimensions.height, mMsaaLevel, false, true, true, true);
        }
        // The MSAA resolve target must also exist when a whole-frame pillarbox is required: the
        // epilogue resolves into it and publishes it as mGfxFrameBuffer so DrawGame composites the
        // centred 4:3 region. Resolving straight to the window would stretch 4:3 content across it.
        if (mMsaaLevel > 1 && (!ViewportMatchesRendererResolution() || widescreenPillarbox)) {
            mRapi->UpdateFramebufferParameters(mGameFbMsaaResolved, mCurDimensions.width, mCurDimensions.height, 1,
                                               false, false, false, false);
            sGameFbMsaaResolvedSized = true;
        }
    } else {
        mRendersToFb = false;
    }

    mFbActive = false;
}

GfxExecStack g_exec_stack = {};

void Interpreter::RunGuiOnly() {
    SpReset();

    // Unlike Run(), this path does not re-latch mForceFixedAspectCache, so there is no staleness
    // gap (mode flips arrive with a game task, via Run()). If a re-latch is ever added here, it
    // must also recompute mRendersToFb the way Run()'s prologue does.

    mGetPixelDepthPending.clear();
    mGetPixelDepthCached.clear();

    mRapi->UpdateFramebufferParameters(0, mGfxCurrentWindowDimensions.width, mGfxCurrentWindowDimensions.height, 1,
                                       false, true, true, !mRendersToFb);
    mRapi->StartFrame();
    mRapi->StartDrawToFramebuffer(mRendersToFb ? mGameFb : 0, (float)mCurDimensions.height / mNativeDimensions.height);
    mRapi->ClearFramebuffer(true, true);
    mRdp->viewport_or_scissor_changed = true;
    mRenderingState.viewport = {};
    mRenderingState.scissor = {};

    Flush();
    mGfxFrameBuffer = 0;

    if (mRendersToFb) {
        const bool widescreenPillarbox = !mWidescreenEnabledCache || mForceFixedAspectCache;
        mRapi->StartDrawToFramebuffer(0, 1);
        mRapi->ClearFramebuffer(true, true);
        if (mMsaaLevel > 1) {
            if (ViewportMatchesRendererResolution() && !widescreenPillarbox) {
                // Normal path: resolve straight to the window; mGfxFrameBuffer stays
                // 0 and DrawGame presents it directly.
                mRapi->ResolveMSAAColorBuffer(0, mGameFb);
            } else if (sGameFbMsaaResolvedSized) {
                // Resolve into the offscreen target and publish it so DrawGame
                // composites (pillarboxes) it instead of stretching.
                mRapi->ResolveMSAAColorBuffer(mGameFbMsaaResolved, mGameFb);
                mGfxFrameBuffer = (uintptr_t)mRapi->GetFramebufferTextureId(mGameFbMsaaResolved);
            } else {
                // A config flip after StartFrame requested the offscreen resolve, but
                // mGameFbMsaaResolved was never sized this frame; resolve to 0 rather
                // than publish an unallocated framebuffer's texture id.
                mRapi->ResolveMSAAColorBuffer(0, mGameFb);
            }
        } else {
            mGfxFrameBuffer = (uintptr_t)mRapi->GetFramebufferTextureId(mGameFb);
        }
    } else if (mFbActive) {
        // Failsafe reset to main framebuffer to prevent softlocking the renderer
        mFbActive = 0;
        mRapi->StartDrawToFramebuffer(0, 1);

        assert(0 && "active framebuffer was never reset back to original");
    }
}

void (*Interpreter::sPortAfterClearHook)(Interpreter*) = nullptr;

void Interpreter::SetPortAfterClearHook(void (*hook)(Interpreter*)) {
    sPortAfterClearHook = hook;
}

void Interpreter::Run(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtx_replacements) {
    // [prof] RUN: the whole DL execution. Self time (minus the VTX/TRI/IMP/DRW/MTX
    // children) = per-command dispatch + every un-instrumented handler — the
    // "dispatch remainder" column (dsp=). One scope per task/sub-frame pass.
    GDX_PROF_SCOPE(RUN);
    SpReset();

    // Re-latch the fixed-aspect flag per task: the game can flip its mode between this frame's
    // StartFrame and the gfx task that renders the new mode, so the StartFrame latch alone applies
    // the old mode's aspect to the new mode's first frame -- a 4:3 squeeze on editor exit.
    mForceFixedAspectCache = sGdxForceFixedAspect != 0;

    // Recompute the render-target decision with the fresh flag: StartFrame latched mRendersToFb
    // before this frame's dispatch, and a mid-dispatch mode flip would otherwise leave vertices on
    // the new mode while rendering to the old target (one squashed, un-pillarboxed frame).
    // Prologue and epilogue both read the mRendersToFb set here. Only mMsaaLevel <= 1 can flip it,
    // since MSAA forces it true in StartFrame; a flip that stales the MSAA resolve sizing is
    // caught by the epilogue's fallback branch.
    {
        const bool widescreenPillarbox = !mWidescreenEnabledCache || mForceFixedAspectCache;
        const bool rendersToFb = !ViewportMatchesRendererResolution() || mMsaaLevel > 1 || widescreenPillarbox;
        if (rendersToFb && !mRendersToFb && mMsaaLevel <= 1) {
            // StartFrame never sized mGameFb this frame; reproduce its non-MSAA
            // sizing so the freshly-targeted offscreen render matches the window.
            mRapi->UpdateFramebufferParameters(mGameFb, mCurDimensions.width, mCurDimensions.height, mMsaaLevel, true,
                                               true, true, true);
        }
        mRendersToFb = rendersToFb;
    }

    mGetPixelDepthPending.clear();
    mGetPixelDepthCached.clear();

    mCurMtxReplacements = &mtx_replacements;

    mRapi->UpdateFramebufferParameters(0, mGfxCurrentWindowDimensions.width, mGfxCurrentWindowDimensions.height, 1,
                                       false, true, true, !mRendersToFb);
    mRapi->StartFrame();
    mRapi->StartDrawToFramebuffer(mRendersToFb ? mGameFb : 0, (float)mCurDimensions.height / mNativeDimensions.height);
    mRapi->ClearFramebuffer(true, true);
    mRdp->viewport_or_scissor_changed = true;
    mRenderingState.viewport = {};
    mRenderingState.scissor = {};

    // Runs on the freshly-cleared canvas, before the task's commands, so its content (the
    // boot-logo CPU framebuffer) draws as a background under whatever this task renders.
    if (sPortAfterClearHook != nullptr) {
        sPortAfterClearHook(this);
        // The seed draws a full-frame copy-mode rectangle, which dirties the tracked
        // viewport/scissor state; reset so the task's first command re-applies its own.
        mRdp->viewport_or_scissor_changed = true;
        mRenderingState.viewport = {};
        mRenderingState.scissor = {};
    }

    auto dbg = mGfxDebugger;
    g_exec_stack.start((F3DGfx*)commands);
    while (!g_exec_stack.cmd_stack.empty()) {
        auto cmd = g_exec_stack.cmd_stack.top();

        if (dbg->IsDebugging()) {
            g_exec_stack.gfx_path.push_back(cmd);
            if (dbg->HasBreakPoint(g_exec_stack.gfx_path)) {
                // On a breakpoint with the active framebuffer still set, we need to reset back to prevent
                // soft locking the renderer
                if (mFbActive) {
                    mFbActive = 0;
                    mRapi->StartDrawToFramebuffer(mRendersToFb ? mGameFb : 0, 1);
                }

                break;
            }
            g_exec_stack.gfx_path.pop_back();
        }
        gfx_step();
    }

    Flush();

    mGfxFrameBuffer = 0;
    // LEAK-HARDENING (3DS first hardware OOM): this reset was spelled
    // `currentDir = std::stack<std::string>()`, which constructs a brand-new backing
    // deque — libstdc++ allocates its node map plus one 504-byte node buffer
    // (512 / sizeof(std::string) = 21 slots on ILP32) — and frees the old one, EVERY
    // task. The failing allocation of the first on-hardware OOM was exactly that
    // 504-byte node (std::deque<std::string>::_M_create_nodes) once session-long heap
    // fragmentation could no longer serve it. Pop in place instead: zero allocations
    // when the stack is already empty (this port's o2r command stream never emits
    // OTR_G_PUSHCD, so it always is), pure deallocation otherwise, and the stack is
    // empty afterwards either way.
    while (!currentDir.empty()) {
        currentDir.pop();
    }

    if (mRendersToFb) {
        // Recompute from the caches as they stand at task end: the prologue re-latch
        // means this reflects the mode actually rendered, not StartFrame's latch.
        const bool widescreenPillarbox = !mWidescreenEnabledCache || mForceFixedAspectCache;
        mRapi->StartDrawToFramebuffer(0, 1);
        mRapi->ClearFramebuffer(true, true);
        if (mMsaaLevel > 1) {
            if (ViewportMatchesRendererResolution() && !widescreenPillarbox) {
                // Normal path: resolve straight to the window; mGfxFrameBuffer stays
                // 0 and DrawGame presents it directly.
                mRapi->ResolveMSAAColorBuffer(0, mGameFb);
            } else if (sGameFbMsaaResolvedSized) {
                // Resolve into the offscreen target and publish it so DrawGame
                // composites the centred 4:3 region (gated on mGfxFrameBuffer != 0).
                mRapi->ResolveMSAAColorBuffer(mGameFbMsaaResolved, mGameFb);
                mGfxFrameBuffer = (uintptr_t)mRapi->GetFramebufferTextureId(mGameFbMsaaResolved);
            } else {
                // The mode flipped to pillarbox after StartFrame, so the offscreen
                // target is not allocated this frame. Resolve to 0 for one frame
                // rather than publish an unallocated framebuffer; the next
                // StartFrame sizes it.
                mRapi->ResolveMSAAColorBuffer(0, mGameFb);
            }
        } else {
            mGfxFrameBuffer = (uintptr_t)mRapi->GetFramebufferTextureId(mGameFb);
        }
    } else if (mFbActive) {
        // Failsafe reset to main framebuffer to prevent softlocking the renderer
        mFbActive = 0;
        mRapi->StartDrawToFramebuffer(0, 1);

        assert(0 && "active framebuffer was never reset back to original");
    }
}

void Interpreter::EndFrame() {
    mRapi->EndFrame();
    mWapi->SwapBuffersBegin();
    mRapi->FinishRender();
    mWapi->SwapBuffersEnd();
}

void gfx_set_target_ucode(UcodeHandlers ucode) {
    ucode_handler_index = ucode;
    gfx_rebuild_flat_dispatch(); // [perf-dispatch] table tracks every ucode_handler_index write
}

int Interpreter::GetTargetFps() {
    return mWapi->GetTargetFps();
}

void Interpreter::SetTargetFps(int fps) {
    mWapi->SetTargetFps(fps);
}

void Interpreter::SetMaxFrameLatency(int latency) {
    mWapi->SetMaxFrameLatency(latency);
}

int Interpreter::CreateFrameBuffer(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                                   uint8_t resize, bool forceFixedAspect) {
    uint32_t orig_width = width, orig_height = height;
    if (resize) {
        AdjustWidthHeightForScale(width, height, native_width, native_height);
    }

    int fb = mRapi->CreateFramebuffer();
    mRapi->UpdateFramebufferParameters(fb, width, height, 1, true, true, true, true);

    mFrameBuffers[fb] = {
        orig_width, orig_height, width, height, native_width, native_height, static_cast<bool>(resize), forceFixedAspect
    };
    return fb;
}

void Interpreter::SetFrameBuffer(int fb, float noiseScale) {
    mRapi->StartDrawToFramebuffer(fb, noiseScale);
    mRapi->ClearFramebuffer(false, true);
}

void Interpreter::CopyFrameBuffer(int fb_dst_id, int fb_src_id, bool copyOnce, bool* hasCopiedPtr) {
    // Do not copy again if we have already copied before
    if (copyOnce && hasCopiedPtr != nullptr && *hasCopiedPtr) {
        return;
    }

    if (fb_src_id == 0 && mRendersToFb) {
        // read from the framebuffer we've been rendering to
        fb_src_id = mGameFb;
    }

    int srcX0, srcY0, srcX1, srcY1;
    int dstX0, dstY0, dstX1, dstY1;

    // When rendering to the main window buffer or MSAA is enabled with a buffer size equal to the view port,
    // then the source coordinates must account for any docked ImGui elements
    if (fb_src_id == 0 || (mMsaaLevel > 1 && mCurDimensions.width == mGameWindowViewport.width &&
                           mCurDimensions.height == mGameWindowViewport.height)) {
        srcX0 = mGameWindowViewport.x;
        srcY0 = mGameWindowViewport.y;
        srcX1 = mGameWindowViewport.x + mGameWindowViewport.width;
        srcY1 = mGameWindowViewport.y + mGameWindowViewport.height;
    } else {
        srcX0 = 0;
        srcY0 = 0;
        srcX1 = mCurDimensions.width;
        srcY1 = mCurDimensions.height;
    }

    dstX0 = 0;
    dstY0 = 0;
    dstX1 = mCurDimensions.width;
    dstY1 = mCurDimensions.height;

    mRapi->CopyFramebuffer(fb_dst_id, fb_src_id, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1);

    // Set the copied pointer if we have one
    if (hasCopiedPtr != nullptr) {
        *hasCopiedPtr = true;
    }
}

void Interpreter::ResetFrameBuffer() {
    mRapi->StartDrawToFramebuffer(0, (float)mCurDimensions.height / mNativeDimensions.height);
}

void Interpreter::AdjustPixelDepthCoordinates(float& x, float& y) {
    x = x * RATIO_X(mActiveFrameBuffer, mCurDimensions) -
        (mNativeDimensions.width * RATIO_X(mActiveFrameBuffer, mCurDimensions) - mCurDimensions.width) / 2;
    y *= RATIO_Y(mActiveFrameBuffer, mCurDimensions);
    if (!mRendersToFb || (mMsaaLevel > 1 && mCurDimensions.width == mGameWindowViewport.width &&
                          mCurDimensions.height == mGameWindowViewport.height)) {
        x += mGameWindowViewport.x;
        y += mGfxCurrentWindowDimensions.height - (mGameWindowViewport.y + mGameWindowViewport.height);
    }
}

void Interpreter::GetPixelDepthPrepare(float x, float y) {
    AdjustPixelDepthCoordinates(x, y);
    mGetPixelDepthPending.emplace(x, y);
}

uint16_t Interpreter::GetPixelDepth(float x, float y) {
    AdjustPixelDepthCoordinates(x, y);

    if (auto it = mGetPixelDepthCached.find(std::make_pair(x, y)); it != mGetPixelDepthCached.end()) {
        return it->second;
    }

    mGetPixelDepthPending.emplace(x, y);

    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res =
        mRapi->GetPixelDepth(mRendersToFb ? mGameFb : 0, mGetPixelDepthPending);
    mGetPixelDepthCached.merge(res);
    mGetPixelDepthPending.clear();

    return mGetPixelDepthCached.find(std::make_pair(x, y))->second;
}

void gfx_push_current_dir(char* path) {
    if (gfx_check_image_signature(path) == 1)
        path = &path[7];

    currentDir.push(GetPathWithoutFileName(path));
}

int32_t gfx_check_image_signature(const char* imgData) {
    uintptr_t i = (uintptr_t)(imgData);

    if ((i & 1) == 1) {
        return 0;
    }

    // Filter addresses that are obviously not valid string pointers before
    // attempting to dereference for the "__OTR__" check.
    if (i == 0 || i < 0x10000) {
        return 0;
    }
#if UINTPTR_MAX > 0xFFFFFFFFu
    // On 64-bit: filter kernel/sentinel addresses. Upper bound covers all
    // user-space layouts (x86_64 47-bit canonical, ARM64 48-bit VA, etc.).
    if (i > 0x0000FFFFFFFFFFFFull) {
        return 0;
    }
#endif

#ifdef _WIN32
    // Plausible-looking but unmapped pointers still reach here (stale SETTIMG
    // addresses during screen transitions crashed OtrSignatureCheck's strncmp).
    // Verify the memory is actually committed and readable first.
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(imgData), &mbi, sizeof(mbi)) == 0 ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return 0;
        }
    }
#endif

    return Ship::Context::GetInstance()->GetResourceManager()->OtrSignatureCheck(imgData);
}

void Interpreter::RegisterBlendedTexture(const char* name, uint8_t* mask, uint8_t* replacement) {
    if (gfx_check_image_signature(name)) {
        name += 7;
    }

    if (gfx_check_image_signature(reinterpret_cast<char*>(replacement))) {
        Fast::Texture* tex = std::static_pointer_cast<Fast::Texture>(
                                 Ship::Context::GetInstance()->GetResourceManager()->LoadResourceProcess(
                                     reinterpret_cast<char*>(replacement)))
                                 .get();

        replacement = tex->ImageData;
    }

    mMaskedTextures[name] = MaskedTextureEntry{ mask, replacement };
}

void Interpreter::UnregisterBlendedTexture(const char* name) {
    if (gfx_check_image_signature(name)) {
        name += 7;
    }

    mMaskedTextures.erase(name);
}

// New getters and setters
void Interpreter::SetNativeDimensions(float width, float height) {
    mNativeDimensions.width = width;
    mNativeDimensions.height = height;
}

void Interpreter::SetResolutionMultiplier(float multiplier) {
    mCurDimensions.internal_mul = multiplier;
}

void Interpreter::SetMsaaLevel(uint32_t level) {
    mMsaaLevel = level;
}

void Interpreter::GetCurDimensions(uint32_t* width, uint32_t* height) {
    *width = mCurDimensions.width;
    *height = mCurDimensions.height;
}

} // namespace Fast

void gfx_cc_get_features(uint64_t shader_id0, uint64_t shader_id1, struct CCFeatures* cc_features) {
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            for (int k = 0; k < 4; k++) {
                cc_features->c[i][j][k] = shader_id0 >> i * 32 + j * 16 + k * 4 & 0xf;
            }
        }
    }

    cc_features->opt_alpha = (shader_id1 & SHADER_OPT(ALPHA)) != 0;
    cc_features->opt_fog = (shader_id1 & SHADER_OPT(FOG)) != 0;
    cc_features->opt_texture_edge = (shader_id1 & SHADER_OPT(TEXTURE_EDGE)) != 0;
    cc_features->opt_noise = (shader_id1 & SHADER_OPT(NOISE)) != 0;
    cc_features->opt_2cyc = (shader_id1 & SHADER_OPT(_2CYC)) != 0;
    cc_features->opt_alpha_threshold = (shader_id1 & SHADER_OPT(ALPHA_THRESHOLD)) != 0;
    cc_features->opt_invisible = (shader_id1 & SHADER_OPT(INVISIBLE)) != 0;
    cc_features->opt_grayscale = (shader_id1 & SHADER_OPT(GRAYSCALE)) != 0;
    cc_features->opt_prim_depth = (shader_id1 & SHADER_OPT(PRIM_DEPTH)) != 0;

    cc_features->clamp[0][0] = shader_id1 & SHADER_OPT(TEXEL0_CLAMP_S);
    cc_features->clamp[0][1] = shader_id1 & SHADER_OPT(TEXEL0_CLAMP_T);
    cc_features->clamp[1][0] = shader_id1 & SHADER_OPT(TEXEL1_CLAMP_S);
    cc_features->clamp[1][1] = shader_id1 & SHADER_OPT(TEXEL1_CLAMP_T);

    cc_features->usedTextures[0] = false;
    cc_features->usedTextures[1] = false;
    cc_features->used_masks[0] = false;
    cc_features->used_masks[1] = false;
    cc_features->used_blend[0] = false;
    cc_features->used_blend[1] = false;
    cc_features->numInputs = 0;

    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                if (cc_features->c[c][i][j] >= SHADER_INPUT_1 && cc_features->c[c][i][j] <= SHADER_INPUT_7) {
                    if (cc_features->c[c][i][j] > cc_features->numInputs) {
                        cc_features->numInputs = cc_features->c[c][i][j];
                    }
                }
                if (cc_features->c[c][i][j] == SHADER_TEXEL0 || cc_features->c[c][i][j] == SHADER_TEXEL0A) {
                    cc_features->usedTextures[0] = true;
                    if (cc_features->opt_2cyc) {
                        cc_features->usedTextures[1] = true;
                    }
                }
                if (cc_features->c[c][i][j] == SHADER_TEXEL1 || cc_features->c[c][i][j] == SHADER_TEXEL1A) {
                    cc_features->usedTextures[1] = true;
                    if (cc_features->opt_2cyc) {
                        cc_features->usedTextures[0] = true;
                    }
                }
            }
        }
    }

    for (int c = 0; c < 2; c++) {
        cc_features->do_single[c][0] = cc_features->c[c][0][2] == SHADER_0;
        cc_features->do_single[c][1] = cc_features->c[c][1][2] == SHADER_0;
        cc_features->do_multiply[c][0] = cc_features->c[c][0][1] == SHADER_0 && cc_features->c[c][0][3] == SHADER_0;
        cc_features->do_multiply[c][1] = cc_features->c[c][1][1] == SHADER_0 && cc_features->c[c][1][3] == SHADER_0;
        cc_features->do_mix[c][0] = cc_features->c[c][0][1] == cc_features->c[c][0][3];
        cc_features->do_mix[c][1] = cc_features->c[c][1][1] == cc_features->c[c][1][3];
        cc_features->color_alpha_same[c] = (shader_id0 >> c * 32 & 0xffff) == (shader_id0 >> c * 32 + 16 & 0xffff);
    }

    if (cc_features->usedTextures[0] && shader_id1 & SHADER_OPT(TEXEL0_MASK)) {
        cc_features->used_masks[0] = true;
    }
    if (cc_features->usedTextures[1] && shader_id1 & SHADER_OPT(TEXEL1_MASK)) {
        cc_features->used_masks[1] = true;
    }

    if (cc_features->usedTextures[0] && shader_id1 & SHADER_OPT(TEXEL0_BLEND)) {
        cc_features->used_blend[0] = true;
    }
    if (cc_features->usedTextures[1] && shader_id1 & SHADER_OPT(TEXEL1_BLEND)) {
        cc_features->used_blend[1] = true;
    }

    cc_features->shader_id = Fast::ShaderIdUnmask(shader_id1);
}

extern "C" int gfx_create_framebuffer(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                                      uint8_t resize, bool forceFixedAspect) {
    return Fast::mInstance.lock().get()->CreateFrameBuffer(width, height, native_width, native_height, resize,
                                                           forceFixedAspect);
}

extern "C" void gfx_texture_cache_clear() {
    Fast::mInstance.lock().get()->TextureCacheClear();
}

extern "C" void gfx_shader_cache_clear() {
    Fast::GfxS7InvalidateDrawMemos(); // [perf-s7] comb/prg pointers freed below
    auto instance = Fast::mInstance.lock().get();
    instance->mColorCombinerPool.clear();
    instance->mPrevCombiner = Fast::mInstance.lock().get()->mColorCombinerPool.end();
    instance->mRenderingState.mShaderProgram = nullptr;
    instance->mRapi->ClearShaderCache();
}

extern "C" void gfx_register_fb_texture(const void* cpuAddr, int fbId) {
    Fast::mInstance.lock().get()->RegisterFbTexture(cpuAddr, fbId);
}

extern "C" void gfx_unregister_fb_texture(const void* cpuAddr) {
    Fast::mInstance.lock().get()->UnregisterFbTexture(cpuAddr);
}
