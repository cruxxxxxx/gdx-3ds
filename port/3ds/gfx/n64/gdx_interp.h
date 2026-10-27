/* port/3ds/gfx/n64/gdx_interp.h -- the owned F3DEX2 interpreter (plan 06, Tier 1).
 *
 * One instance lives on the render thread. RunFrame walks the task's Gfx stream through
 * DlSrc, keeps the RSP/RDP state mirrors below (shaped like the LUS structs wherever parity
 * depends on them, plan §3.4) and drives the citro3d backend through gdxn::Backend.
 *
 * TUs: gdx_dl.cpp (walker), gdx_interp.cpp (frame, pure-state handlers), gdx_rsp.cpp (matrices,
 * lights, T&L), gdx_tmem.cpp (TMEM spans + loads), gdx_texcache.cpp (content cache + decoders),
 * gdx_tri.cpp (state diff, batch emission), gdx_rect.cpp (viewport/scissor/rects), gdx_cc.cpp.
 * Milestone state: docs/research/native/interp-progress.md. */
#ifndef GDX_INTERP_H
#define GDX_INTERP_H

#include <stddef.h>
#include <new>
#include <stdint.h>

#include "gdx_backend.h"
#include "gdx_cc.h"
#include "gdx_dlsrc.h"
#include "gdx_dlsrc_bridge.h"
#include "gdx_gbi.h"

typedef struct GdxGfxJob GdxGfxJob;
struct GdxHudWord; /* gdx3ds_hud_record.h: one word of a captured HUD run */
struct GdxHudRun;  /* gdx3ds_hud_record.h: one captured run */
struct NrrStateKey; /* ../nrr/gdx3ds_nrr.h: a core state-block key (the NRR HUD interns them) */
struct NrrState;    /* ../nrr/gdx3ds_nrr.h: a core state block (n60hud3: the HUD builds its own when the table is full) */
class GdxOwnedSeam; /* gdx_seam_owned.cpp: the native replay seam over this interpreter */
struct Gdx3dsCourseModel; /* gdx3ds_interp_seam.h: a baked course preview model (nicourse) */

namespace gdxn {

constexpr int kMaxLights = 32;
constexpr int kMaxVertices = 64;
constexpr unsigned kNrrMemoKeyWords = 16; /* gdx_nrr_hud.cpp: a memoised HUD rectangle's inputs */
constexpr int kMatrixStack = 11;
constexpr int kCallStack = 32;
constexpr int kMaxTriBuffer = 256;
constexpr unsigned kTexCacheSize = 1024;
constexpr unsigned kTexCacheHash = 2048;
/* Decode scratch. kTexUploadBytes is the INITIAL size (the historic fixed 256 KiB, a plan
 * deviation: LUS has a flat 4 MiB). DROPSITES: it used to be a hard ceiling -- a texture whose
 * decode needed more was skipped, and because ImportTexture marked the entry uploaded anyway,
 * the cache id kept whatever the previous owner of that backend id had drawn. The scratch now
 * grows on demand up to kTexUploadMaxBytes, which is the largest upload the PICA backend can
 * accept at all (GfxRenderingAPIC3D::UploadTexture refuses padW/padH > 1024, so 1024*1024
 * RGBA8 is the true ceiling). Above that no decode could ever have been displayed, so the
 * import takes the neutral fallback instead of a stale sample. */
constexpr uint32_t kTexUploadBytes = 256u * 1024u;
constexpr uint32_t kTexUploadMaxBytes = 1024u * 1024u * 4u;
/* The neutral fallback an undecodable import uploads in place of a stale sample: opaque white,
 * 8x8 because 8 is the PICA minimum side (UploadTexture pads below it and the padding would
 * otherwise be transparent black wherever the UVs ran past the single texel). */
constexpr uint32_t kFallbackTexDim = 8u;
constexpr uint32_t kFallbackTexBytes = kFallbackTexDim * kFallbackTexDim * 4u;

/* N64 Light_t / lookat / point light, host-endian 16 bytes (LUS F3DLight union). */
struct Light {
    uint8_t col[3];
    uint8_t pad1;
    uint8_t colc[3];
    uint8_t pad2;
    int8_t dir[3];
    uint8_t pad3;
    uint8_t pad4[4];
};
struct PointLight {
    uint8_t col[3];
    uint8_t unk3;
    uint8_t colc[3];
    uint8_t unk7;
    int16_t pos[3];
    uint8_t unkE;
    uint8_t pad;
};
union LightU {
    Light l;
    PointLight p;
};

/* N64 Vtx_t, 16 bytes; host-endian in wide/host-N64 lists, big-endian in raw-BE lists. */
struct Vtx {
    int16_t ob[3];
    uint16_t flag;
    int16_t tc[2];
    uint8_t cn[4];
};

struct Rgba {
    uint8_t r, g, b, a;
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    Rgba color;
    uint8_t clip_rej;
    uint8_t clip_out;
};

struct Rect {
    int16_t x, y;
    uint32_t width, height;
};

struct Rsp {
    float modelview_matrix_stack[kMatrixStack][4][4];
    uint8_t modelview_matrix_stack_size;
    float MP_matrix[4][4];
    float P_matrix[4][4];

    Light lookat[2];
    LightU current_lights[kMaxLights + 1];
    float current_lights_coeffs[kMaxLights][3];
    float current_lookat_coeffs[2][3];
    uint8_t current_num_lights; /* includes ambient */
    bool lights_changed;

    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    uint32_t extra_geometry_mode;

    float viewport_z_scale;
    float viewport_z_trans;

    struct {
        uint16_t s, t; /* U0.16 */
    } texture_scaling_factor;

    LoadedVertex loaded_vertices[kMaxVertices + 4];

    uint8_t dmem[4096];
    uint16_t dma_io_dmem;
    bool dma_io_loaded;
    float f3dflx_alpha_light[3];
    bool f3dflx_alpha_light_valid;
};

struct Tile {
    uint8_t fmt, siz;
    uint8_t cms, cmt;
    uint8_t masks, maskt;
    uint8_t shifts, shiftt;
    float uls, ult, lrs, lrt;
    uint16_t tmem; /* 64-bit word units */
    uint32_t line_size_bytes;
    uint8_t palette;
    uint16_t tmem_index;
};

/* One TMEM load record (LUS LoadedTexture minus the resource handle). `immutable` is the
 * LUS "resource-backed" class (o2r-keyed asset-segment texture): legacy decode from the source
 * bytes, same-content skip eligible. `native_rgba16` texels are host-order (byte-swapped into
 * the mirror, the way CopyRawTextureBytes fed LUS). */
struct LoadedTexture {
    const uint8_t* addr;
    uint32_t orig_size_bytes;
    uint32_t size_bytes;
    uint32_t full_image_line_size_bytes;
    uint32_t line_size_bytes;
    uint16_t tmem_start;
    uint16_t tmem_word_count;
    uint8_t immutable;
    uint8_t native_rgba16;
    uint8_t masked;  /* always 0 on this port (no replacement packs) */
    uint8_t blended; /* always 0 */
};

struct Rdp {
    const uint8_t* palettes[2];
    const uint8_t* palette_dram_addr[2];
    uint8_t palette_staging[2][256];
    struct {
        const uint8_t* addr; /* resolved lazily at load time */
        uint8_t fmt;
        uint8_t siz;
        uint32_t width;
        uint8_t immutable;
        uint8_t native_rgba16;
        Cmd cmd; /* the SETTIMG operand token */
    } texture_to_load;

    /* Span model (plan §3.6): base slots hold the record; interior words derive lazily. */
    LoadedTexture loaded_texture[512];
    uint16_t tmem_span_base[512];  /* 0 = none, else base + 1 */
    uint16_t tmem_span_words[512]; /* valid at a base */
    uint8_t tmem_span_load_kind[512];
    /* [tmemblock] the SOURCE-STABILITY stamp of the last mirror written at this base:
     * [0] gNativeRgba16Generation, [1] gDmaGeneration -- sampled BEFORE the copy -- and the
     * source class they were sampled under. Read only at a base whose span is still live and
     * whose descriptor still matches; every path that retires a span already zeroes
     * tmem_span_words[base], which SameContentDescriptor tests before the stamp is consulted.
     * 8.5 KiB on the one heap-allocated Interp (gdx_rt_glue.cpp:684). tmemblock-progress.md S2. */
    uint64_t tmem_span_src_stamp[512][2];
    uint8_t tmem_span_src_cls[512];
    uint16_t tmem_span_list[64];
    uint16_t tmem_span_list_n; /* 0xFFFF = invalid (rebuild) */
    alignas(8) uint8_t tmem[4096];
    uint32_t tmem_generation;

    Tile texture_tile[8];
    bool textures_changed[2];
    uint8_t first_tile_index;

    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    bool grayscale;
    uint8_t prim_lod_fraction;
    uint16_t prim_depth;
    Rgba env_color, prim_color, fog_color, blend_color, fill_color, grayscale_color;
    Rgba key_center, key_scale;
    int16_t convert_k[6];

    Rect viewport, scissor;
    bool viewport_or_scissor_changed;
    uintptr_t z_buf_address;
    uintptr_t color_image_address;
};

/* Texture cache key: content, never identity (plan §3.7). Mirrors TextureCacheKey. */
struct TexKey {
    const uint8_t* addr;
    const uint8_t* pal[2];
    uint8_t fmt, siz, palette_index;
    uint32_t size_bytes;
    uint32_t line_size_bytes;
    uint32_t full_image_line_size_bytes;
    uint16_t tile_width, tile_height;
    uint8_t cms, cmt, masks, maskt;
    uint32_t content_hash;
    uint32_t palette_content_hash; /* [native] palhash; 0 when the lever is off or fmt is not CI */
};

struct TexEntry {
    TexKey key;
    uint32_t texture_id;
    bool uploaded;
    bool used;
    int16_t lruPrev, lruNext;
    int16_t hashNext;
    uint16_t gen; /* N60TEX [debug] tex_keep: the cache generation (mode) that last used it */
};

/* Backend-facing state the last batch was drawn with (the diff set, plan §3.9). */
struct RenderState {
    uint8_t depth_test_and_mask; /* 0xFF = unknown */
    uint8_t decal_mode;          /* 0/1, 0xFF = unknown */
    uint8_t alpha_blend;         /* 0/1, 0xFF = unknown */
    Rect viewport, scissor;
    void* shader_program;
    int16_t textures[2]; /* TexEntry index bound per unit, -1 none */
    bool sampler_valid[2];
    bool sampler_linear[2];
    uint8_t sampler_cms[2];
    uint8_t sampler_cmt[2];
};

/* Why an open batch was cut. One class per cause so the [ncensus] table ranks them:
 * the five ex-FLUSH_CONST register classes are split apart because only some of them can
 * ride per vertex (the prim/env fold), and lumping them hid that. */
enum FlushReason : uint8_t {
    FLUSH_NONE = 0,
    FLUSH_SHADER,
    FLUSH_TEXTURE,
    FLUSH_DEPTH,
    FLUSH_DECAL,
    FLUSH_VIEWPORT,
    FLUSH_SCISSOR,
    FLUSH_ALPHA,
    FLUSH_PRIM,      /* SETPRIMCOLOR value change (incl. prim_lod_fraction) */
    FLUSH_ENV,       /* SETENVCOLOR value change */
    FLUSH_FOGCOLOR,  /* SETFOGCOLOR value change */
    FLUSH_BLENDA,    /* SETBLENDCOLOR alpha change ([native] blendflush) */
    FLUSH_PRIMDEPTH, /* SETPRIMDEPTH value change */
    FLUSH_FBOP,
    FLUSH_FULL,
    FLUSH_TASK_END,
    FLUSH_FOG,      /* G_MW_FOG line change */
    FLUSH_COMBINER,
    FLUSH_SAMPLER,
    FLUSH_HOOK, /* a native replay hook (track/fx marker, machines G_DL) drained the batch */
    FLUSH_POSE, /* [native] replay: the next triangle's matrix-state tag differs (C6) */
    FLUSH_COUNT
};

/* [native] ncensus: batch-size buckets, by triangles in the drained batch. */
enum { kBatchHistBuckets = 8 };

/* [native] dspcensus: where a dispatched command's microseconds go. The walker is ITERATIVE
 * (G_DL pushes a frame and returns), so Exec() never re-enters Walk and per-opcode time is
 * SELF time by construction -- unlike LUS's [profop], whose 06 bucket is inclusive. The
 * already-attributed [prof] child sections (vtx/tri/imp/drw/mtx) are subtracted so the census
 * sums to `dsp`, the dispatch remainder, and not to the whole frame.
 *   DSP_PH_FETCH     ReadCmd + the readable-bound test + the op/cmds bookkeeping + RemapF3d
 *   DSP_PH_DLRESOLVE DlSrc::ResolveList inside G_DL (the bridge seam: segment epoch, source
 *                    translation, KnownCommandLimit, LooksLikeDisplayList, list facts)
 *   DSP_PH_DLHOOK    the registered machines G_DL hook -- **THIS IS NOT DISPATCH.** It is
 *                    DrawMesh: the [native] machines feature doing real drawing work, billed
 *                    to G_DL because the hook is called from the G_DL handler. Its own [prof]
 *                    child sections are subtracted from DE's SELF time but NOT from this
 *                    bucket, so it is inclusive where every other phase here is self, and it
 *                    tracks machine count (16 us/call in the thinnest window, 94 us/call in a
 *                    30-machine storm). The dispatch part of G_DL is DLRESOLVE. Anyone reading
 *                    this census for a dispatch win must skip this line -- dispatch-progress.md
 *                    and loadblock-progress.md both say so, and it is still the biggest number
 *                    in the G_DL row.
 *   DSP_PH_DLPUSH    call-stack push / tail-call bookkeeping
 * Loop overhead is `dsp` minus the sum; it is not a bucket. */
enum DspPhase {
    DSP_PH_FETCH = 0,
    DSP_PH_DLRESOLVE,
    DSP_PH_DLHOOK,
    DSP_PH_DLPUSH,
    /* F3 (G_LOADBLOCK), the census's #1: the per-load source classification and the mirror.
     *   DSP_PH_TIMGRES   DlSrc::ResolveTimg (the bridge's SETTIMG resolution)
     *   DSP_PH_TIMGCLASS TexelsAreNativeRgba16 + TimgIsResource -- the native-RGBA16 range
     *                    scan and the o2r asset lookup, re-run on EVERY load
     *   DSP_PH_TMEMSAME  the same-content skip test
     *   DSP_PH_TMEMCOPY  MirrorToTmem
     *   DSP_PH_TMEMSTORE StoreLoadedTexture + NoteTmemLoadKind (the span store) */
    DSP_PH_TIMGRES,
    DSP_PH_TIMGCLASS,
    DSP_PH_TMEMSAME,
    DSP_PH_TMEMCOPY,
    DSP_PH_TMEMSTORE,
    DSP_PH_COUNT
};

/* [dspx] (o3dsinterp, NEXT.md C4m): the owned walker's `dsp` split by HANDLER GROUP, cheaply
 * enough to leave on in every gputrace run. Unlike [native] dspcensus (three clock reads per
 * command, ~5 ms/frame of its own on an Old 3DS) this times only the low-frequency groups
 * below -- two reads per call, ~600 calls per task -- and each figure is SELF time: the [prof]
 * sections the handler entered (vtx/tri/imp/drw/mtx) are subtracted, exactly as the census
 * does, so the groups sum to part of `dsp` and `dsp - sum` is the per-command walk plus the
 * untimed high-frequency state handlers (colors, othermode, syncs, TRI2 dispatch).
 *   DX_LOAD  G_LOADBLOCK / G_LOADTILE / G_LOADTLUT (resolve, classify, mirror, span store)
 *   DX_DL    G_DL minus its hook: the bridge list resolve + push
 *   DX_MCH   the G_DL hook = [native] machines DrawMesh, SELF (its tnl/bind/emit live here)
 *   DX_TRK   the G_NOOP hook on a track marker (p=0x5A/0x5B) = [native] track replay, SELF
 *   DX_FX    the G_NOOP hook on any other marker (fx 0x5C/0x5D, bookkeeping) = [native] fx, SELF
 *   DX_RECT  G_TEXRECT(FLIP) / G_FILLRECT (the SpTri they call is `tri`, subtracted)
 *   DX_MV    G_MOVEWORD / G_MOVEMEM / G_DMA_IO (segment sets resolve through the bridge)
 * Deliberately NOT timed: G_SETTILE (~500 calls/task) and G_SETTIMG (~250) -- the dispatch
 * census put both at its own noise floor (~1.9 us/call = the clock reads), so a clock pair on
 * each would cost more than the whole group it measures. They stay in `dsp - sum`. */
enum DxGroup {
    DX_LOAD = 0,
    DX_DL,
    DX_MCH,
    DX_TRK,
    DX_FX,
    DX_RECT,
    DX_MV,
    DX_COUNT
};

/* [trix] SpTri's own time split into phases, SAMPLED (one top-level call in 32) so the
 * receipt costs ~0.15 clock reads per triangle instead of five: a clock read is an SVC,
 * 0.62 us on the Old 3DS and ~1.6 us of emulated time, and 1-in-8 measurably inflated `tri` on
 * a 3.7k-triangle screen. Self time, [prof] children (the fan's nested SpTri, ImportTexture,
 * the batch Flush) subtracted; the receipt also subtracts one calibrated clock read per closed
 * phase interval (TriPhN), which is what the interval itself contains.
 *   TRI_PH_SCREEN   entry -> the state block: reject variant, trivial reject, cull
 *   TRI_PH_CLIP     the Sutherland-Hodgman clipper and the fan loop (nested SpTri subtracted)
 *   TRI_PH_RESOLVE  ResolveDrawState (combiner, tile extents, sampler, shader, blend)
 *   TRI_PH_BEGIN    VboPackBegin at a batch's first triangle
 *   TRI_PH_EMIT     the packed emission: aux, UV prep, 3 x 12 floats, fog note */
enum TriPhase {
    TRI_PH_SCREEN = 0,
    TRI_PH_CLIP,
    TRI_PH_RESOLVE,
    TRI_PH_BEGIN,
    TRI_PH_EMIT,
    TRI_PH_COUNT
};

/* GfxSpTri1's resolved draw state: what SpTri's state block leaves behind after binding the
 * backend (combiner, textures, sampler, shader, blend) and before the packed emission. Banked
 * as the tri memo the native replay seam reads (gdx_seam_owned.cpp PackState) and produced
 * on demand for the machines binder (ResolveDrawState). */
struct DrawState {
    Combiner* comb;
    void* prg;
    uint32_t tm;
    uint32_t tex_width[2], tex_height[2], tex_width2[2], tex_height2[2], effective_tile[2];
    bool usedTextures[2]; /* ShaderGetInfo */
    uint8_t numInputs;
    ClipParams clip;
    bool use_alpha, use_fog, use_blend_color;
};

/* [native] machines_mattab (C5 Stage 1b; gdx_tri.cpp MachMatOps): one machine material run's
 * ResolveDrawState, recorded. KEY = every register the resolve reads to compute its targets
 * (combiner key, depth/decal/blend bits, the texel0/1 tiles and their span views, plus the
 * texture-source classification epoch). RESULT = those targets (depth, decal, per-unit sampler,
 * shader, alpha, the DrawState) and, per texture unit, the texture-cache slot the unit's import
 * hit with that entry's key. Opaque to the mesh code (gdx3ds_interp_seam.h GdxSeamMatRec);
 * all-zero = empty. */
struct MachMatRec {
    uint64_t combineMode;
    uint32_t otherModeL, otherModeH, geometryMode;
    uint32_t impEpoch[2]; /* texture-source classification epoch each unit's import was recorded at */
    uint32_t useSeq;      /* last served / banked (the victim choice between a run's ways) */
    uint8_t valid, firstTile, grayscale;
    uint8_t depthTM, decal, useAlpha;
    uint8_t smpKnown[2], smpLinear[2], smpCms[2], smpCmt[2];
    uint8_t importOk[2];
    int16_t slot[2];
    Tile tiles[2];
    LoadedTexture lt[2];
    TexKey texKey[2];
    DrawState ds;
};

/* What one MachMatResolve call did (the mesh code's [mattab] receipt). */
enum MachMatResult : uint8_t {
    MM_HIT = 0,     /* served from the record */
    MM_MISS_EMPTY,  /* no record yet */
    MM_MISS_KEY,    /* a register / tile / span view differs */
    MM_MISS_VP,     /* retired: a hit replays the viewport/scissor diff (kept for the receipt layout) */
    MM_MISS_NOIMP,  /* a unit owes an import the record has no hit for */
    MM_MISS_TEX,    /* the recorded cache slot no longer holds the recorded key */
    MM_MISS_EPOCH,  /* the texture-source classification epoch moved */
    MM_MISS_SMP,    /* a bound unit's sampler target was never recorded */
    MM_FULL,        /* verify mode: the full resolve always runs */
    MM_RESULT_COUNT
};
struct MachMatOutcome {
    uint8_t result;   /* MachMatResult */
    uint8_t binds;    /* backend state calls issued by a hit (depth/decal/select/sampler/shader/alpha) */
    uint8_t banked;   /* the record was (re)written from a full resolve */
    uint8_t checked;  /* verify: the record would have hit and was compared */
    uint8_t mismatch; /* verify: ... and disagreed with the full resolve */
    uint8_t wasValid; /* a miss on a record that had been valid (an invalidation) */
    uint16_t keyBits; /* MM_MISS_KEY: 1 combine 2 omL 4 omH 8 geometry 10 first tile 20 grayscale,
                         40/80 unit 0/1 tile, 100/200 unit 0/1 span view */
    uint8_t live;     /* machines_mattab_live: units the hit (or verify's would-hit) imported live */
};

struct Counters {
    uint32_t op[256];
    uint32_t listsByClass[3]; /* Wide, HostN64, RawBE */
    uint32_t listsF3d;
    uint32_t lists;
    uint32_t cmds;
    uint32_t tris;       /* submitted (non-rect) */
    uint32_t trisEmitted;
    uint32_t trisRejected;
    uint32_t trisCulled;
    uint32_t vtx;
    uint32_t batches;
    uint32_t packRefused;
    uint32_t packSplit; /* GEOMLOSS: begins capped below kMaxTriBuffer by pool pressure */
    uint32_t flush[FLUSH_COUNT];
    /* --- [native] ncensus (inert when the lever is off) --- */
    uint32_t flushDrain[FLUSH_COUNT]; /* the subset of flush[] that actually drained tris */
    uint64_t flushTicks[FLUSH_COUNT]; /* svcGetSystemTick spent inside those drains */
    uint32_t batchHist[kBatchHistBuckets];
    uint32_t batchTris;               /* sum of tris over drained batches */
    uint32_t maxBatchesPerTask;
    uint32_t foldable[2];             /* prim / env splits the const-input rule would absorb */
    uint32_t folded[2];               /* prim / env splits actually absorbed ([native] ownfold) */
    uint32_t unknownOps;
    uint32_t droppedOps;
    uint32_t stackOverflow;
    uint32_t runaway;
    uint32_t noopLists;
    uint32_t branchZTaken;
    uint32_t tasks;
    uint32_t rootRejects;
    uint32_t texImports, texHits, texMisses, texEvicts, texDecodeOverflow;
    uint32_t texClears, texInvalidates, texInvalidated;
    /* --- DROPSITES: the texture-cache silent-loss site (gdx_texcache.cpp UploadPrepare). --- */
    uint32_t texDecodeGrow;     /* scratch reallocations (a texture that used to be skipped) */
    uint32_t texNoUpload;       /* imports that reached the end of the decode switch with no upload */
    uint32_t texFallback;       /* neutral fallback uploads issued in place of a stale sample */
    /* --- DROPSITES: G_VTX past kMaxVertices (gdx_rsp.cpp SpVertex). --- */
    uint32_t vtxOver;           /* G_VTX loads whose [dest, dest+n) left the 64-slot window */
    uint32_t vtxOverDropped;    /* vertices those loads did NOT transform */
    /* --- DROPSITES: clip-polygon truncation (gdx_tri.cpp SpTri). --- */
    uint32_t clipTrunc;         /* Sutherland-Hodgman planes that hit the 16-vertex output cap */
    uint32_t tmemLoads, tmemSkips, timgUnresolved;
    /* [native] tmemskip receipt. `tmemDescSame` counts loads whose whole descriptor matched the
       live span (everything SameContentLoad tests EXCEPT the immutable gate) -- i.e. the loads a
       staleness test is even allowed to look at. `tmemStale` are those the test then rejected,
       `tmemSkipImm`/`tmemSkipGen` split the skips by which gate fired, and tmemClsN/S/D/U count
       every load by source class so the class the win actually rides on is visible rather than
       assumed. All inert with the lever off except tmemDescSame/tmemCls*, which the census arms. */
    uint32_t tmemDescSame, tmemStale, tmemSkipImm, tmemSkipGen;
    uint32_t tmemClsN, tmemClsS, tmemClsD, tmemClsU;
    /* WHY the descriptor did not match, first failing test wins. This is the column that says
       whether a staleness test can ever pay: the gate is per TMEM BASE against the load that
       last wrote it, not per texture, so if the game cycles several textures through one base
       inside a task the descriptor never matches and no staleness term can help.
         span   no live span starts at this base (retired by an overlapping load, or never set)
         kind   a span is there, written by the OTHER load kind (LOADBLOCK vs LOADTILE)
         words  a span is there, of a different length
         addr   same span, same length -- a DIFFERENT SOURCE POINTER, i.e. another texture
         other  same pointer, some other descriptor field moved (sizes, strides, swap, o2r) */
    uint32_t tmemMissSpan, tmemMissKind, tmemMissWords, tmemMissAddr, tmemMissOther;
    /* [memotear] [debug] tmemfault receipt (NEXT.md B54): the refusal arm, exercised on
       purpose. tmemFaultGen/tmemFaultCls are the injections made by each mechanism (the source
       generation moved; the class changed under the span); tmemFaultCaught are the ones the
       gate then REFUSED, and tmemFaultMissed the ones it skipped anyway. caught == gen + cls
       and missed == 0 is the claim. All zero unless [debug] tmemfault != 0. */
    uint32_t tmemFaultGen, tmemFaultCls, tmemFaultCaught, tmemFaultMissed;
    uint32_t timgMemoHit, timgMemoMiss, timgMemoInert; /* [native] timgmemo receipt */
    /* Miss BREAKDOWN -- which part of the key failed. A memo that misses is useless in three
       different ways and they have three different fixes, so the receipt says which:
         Empty  the slot was never filled (cold, or thrashed by collisions)
         Epoch  the key matched and the freshness term did not  -> invalidation churn
         Key    the slot held a different operand                -> collision or genuine variety */
    uint32_t timgMemoMissEmpty, timgMemoMissEpoch, timgMemoMissKey;
    uint32_t timgEpochChanges; /* times the class epoch differed from the previous load's */
    uint32_t rects;
    uint32_t vtxUnresolved, mtxUnresolved;
    /* --- [native] dspcensus (inert when the lever is off) --- */
    uint64_t opTicks[256];              /* per-opcode SELF ticks, [prof] child sections removed */
    uint64_t phTicks[DSP_PH_COUNT];     /* walk-phase ticks */
    uint32_t phCalls[DSP_PH_COUNT];
    uint64_t walkTicks;                 /* the whole Walk(), for the unattributed remainder */
    /* --- [dspx] (gputrace; inert otherwise) --- */
    uint64_t dxTicks[DX_COUNT];
    uint32_t dxCalls[DX_COUNT];
    /* Bytes the TMEM loads moved -- the memory traffic an L2-less ARM11 pays per load. Always
       counted (three adds per load). `swap` is the native-RGBA16 subset that is byte-swapped. */
    uint32_t tmemCopyBytes, tmemSwapBytes, spanWords;
    /* --- [trix] SpTri exit paths (always counted) + sampled phase clocks (gputrace) --- */
    uint32_t triCalls, triTopCalls, triRectCalls, triVariantRej, triClipIn, triClipFan, triClipAllOut;
    uint32_t triTrivialRej, triCull, triResolveFull, triResolveMemo, triBegin, triEmitAll;
    uint64_t triPhTicks[TRI_PH_COUNT];
    uint32_t triPhN[TRI_PH_COUNT]; /* closed intervals per phase (clock-cost correction) */
    uint32_t triPhSamples;
};

/* RAII [prof] section; one int test when tracing is off. */
class ProfScope {
  public:
    ProfScope(Backend* b, int sec) : mB(b), mSec(sec), mOn(b->ProfActive()) {
        if (mOn) mT0 = mB->ProfEnter(&mSnap);
    }
    ~ProfScope() {
        if (mOn) mB->ProfExit(mSec, mT0, mSnap);
    }

  private:
    Backend* mB;
    int mSec;
    bool mOn;
    uint64_t mT0 = 0, mSnap = 0;
};

#ifdef EXPANSION_KIT
struct EkPvMemo;  /* gdx_ekpreview.cpp */
struct EkPvEntry; /* gdx_ekpreview.cpp */
struct EkPvCap;   /* gdx_ekpreview.cpp */
#endif

class Interp {
  public:
    explicit Interp(Backend* backend);
    ~Interp();

    void RunFrame(GdxGfxJob* job, const GdxFrameConfig& cfg, int variant);

    const Counters& Stats() const {
        return mStats;
    }
    void ResetStats();
    bool UnknownLoggedOnce(uint8_t op);

    /* Per-opcode call-out for other native tracks (machines: G_DL registered-pointer hook,
     * plan 07 §3.1). fn returns 1 to consume the command (the walker skips the sub-list). */
    typedef int (*DlHook)(void* user, const void* listPtr, Interp* interp);
    void RegisterDlHook(DlHook fn, void* user) {
        mDlHook = fn;
        mDlHookUser = user;
    }
    /* G_NOOP / G_SPNOOP call-out (track + fx marker replay, plan 08 §3.7 / 09 §3.1): the
     * command's two words exactly as the LUS G_NOOP handler receives them. */
    typedef void (*NoopHook)(void* user, uint32_t w0, uint32_t w1, Interp* interp);
    void RegisterNoopHook(NoopHook fn, void* user) {
        mNoopHook = fn;
        mNoopHookUser = user;
    }
    /* nimenu W4 (review §4 hot trims): the G_NOOP/G_SPNOOP marker's two steps -- NrrHudHotClose
     * (a marker hook may draw) then the registered NoopHook itself -- shared between Exec's own
     * OP_NOOP case (gdx_dl.cpp) and NrrRaceHotDrive's marker loop (gdx_nirace.cpp), which used to
     * each inline their own copy. Defined in gdx_dl.cpp. */
    void NrrNoopDispatch(uint32_t w0, uint32_t w1);

    /* --- the native replay seam (gdx_seam_owned.cpp) --- */
    /* G_SETCOMBINE from its raw words (the OP_SETCOMBINE decode). */
    void SetCombineWords(uint32_t w0, uint32_t w1);
    /* G_VTX from an already-resolved source (host-endian unless isBig). */
    void TransformVertices(const uint8_t* src, size_t n, size_t dest, bool isBig);
    /* The full triangle path (gdx_tri.cpp). */
    void SpTri(uint8_t v1, uint8_t v2, uint8_t v3, bool isRect);
    /* SpTri's state block alone: bind the backend for the current registers, no emission. */
    bool ResolveDrawState(bool isRect, DrawState& out);
    /* [native] machines_mattab: ResolveDrawState (isRect = false) served from one of a run's
     * `ways` records when its key matches and its texture slots are still valid, else the full
     * resolve, banked into the matching way (or the least recently used one). mode 1 = use,
     * 2 = verify (always the full resolve, compared with the record that would have served). */
    bool MachMatResolve(MachMatRec* const* recs, int ways, int mode, DrawState& out, MachMatOutcome& o);
    /* [native] track_mattab (C5 Stage 3d; gdx_tri.cpp TrackMatOps): the retained course's
     * per-segment probe (SpTri on the segment's first surviving triangle, emission discarded)
     * served from a course material table: the same MachMatRec proof as the machines, `table`
     * holding nBuckets x ways records bucketed by the register key. `tri` = the probe triangle's
     * three vertices exactly as SpTri would read them; `serve` = the caller proved the reject
     * screen passed and nothing is pending. TrackMatBefore replays SpTri's geometry (clipper, fan,
     * trivial reject, backface) to find whether and from which vertex 0 the state block runs;
     * then, in mode 1: 4 = SpTri would bind and draw nothing (the probe must not run); with a
     * record that validates, it applies the record, opens the packed batch and latches its aux
     * exactly as SpTri would and returns 1 (served), 2 (applied, the batch refused: the probe's
     * triangle is dropped) or 3 (applied, the pool nearly out: the caller runs the stock probe on
     * top). Otherwise 0: the caller probes, then TrackMatAfter banks the record (and in mode 2
     * compares the prediction -- the drop decision, the DrawState, mRs and the aux -- with what
     * the probe did). mode 1 use, 2 verify; bit 0x10 imports a unit the record holds no hit for
     * live (as machines_mattab_live). */
    int TrackMatBefore(MachMatRec* table, int nBuckets, int ways, int mode, bool serve, const LoadedVertex* tri,
                       MachMatOutcome& o);
    /* probe: 0 = the state resolved, 1 = dropped before the state block, 2 = drawn by the interpreter. */
    void TrackMatAfter(MachMatRec* table, int nBuckets, int ways, int mode, int probe, MachMatOutcome& o);
    friend struct MachMatOps;
    friend struct TrackMatOps;
    size_t PendingTris() const {
        return mBufNumTris;
    }
    friend class ::GdxOwnedSeam;

    Rsp& RspState() {
        return mRsp;
    }
    Rdp& RdpState() {
        return mRdp;
    }
    Backend* GetBackend() {
        return mBackend;
    }
    int Variant() const {
        return mVariant;
    }

    /* --- [debug] nicensus (gdx_nicensus.cpp; docs/research/native/nicensus-progress.md) ---
     * Measurement only: what the owned walker still interprets, per game mode and context. With
     * the lever off mNc is null and the walker pays one pointer test per Walk() (the counting
     * loop is a separate body, WalkNicensus, like WalkCensus). Counters and clocks only: it reads
     * the command words and the RDP/RSP registers and never writes interpreter or game state. */
    enum NcCtx {
        NC_WALK = 0,   /* the walker itself (split per frame: between NRR markers / whole screen) */
        NC_NRR = 1,    /* inside an NRR 0x63 marker (and the NRR learn / record markers) */
        NC_NRRHUD = 2, /* a HUD capture run taken by the NRR HUD decoder (nrr_hud) */
        NC_HUD = 3,    /* a HUD / shadow / fx capture run on the stock / direct / retained replay */
        NC_SEAM = 4,   /* the track / fx / gadget / sky marker seams (C5 retained paths) */
        NC_MESH = 5,   /* the G_DL hook: machine meshes, texskip, finish mesh, decorations */
        NC_FRAME = 6,  /* the task outside Walk(): backend frame start / clear / final flush / task ends */
        NC_COUNT = 7
    };
    static constexpr int kNcModes = 24; /* gGameMode 0..0x16, 0x17 = anything else */
    static constexpr int kNcSubs = 7; /* 0 steady, 1 race intro, 2 racing, 3 post-finish, 4 mode change, 5 attract demo,
                                       * 6 a transition drawn ([debug] nicensus_wipe=1 only, menus-s4) */
    struct NcState; /* gdx_nicensus.cpp */
    /* on: 0 off, 1 the engine + its [nicensus] dumps, 2 the engine only (no [nicensus] dumps: the
     * [debug] ni_strict census runs on it without nicensus's own receipts). */
    void NcSet(int on);
    /* --- [debug] ni_strict (gdx_nicensus.cpp; native-all-plan.md S7) ---
     * The strict census rides the nicensus engine: every command an Exec site runs is filed by
     * bucket (mode x sub-scope), context, tag (the marker / hook that opened the context), site (which
     * Exec loop ran it) and opcode, with its per-frame persistence. Level 1 = COUNT: nothing is
     * dropped and nothing drawn changes. Level 2 (drop) is not implemented: it counts like 1. */
    enum NcSite {
        NCS_WALK = 0,      /* the walker's own loop (Walk / WalkNicensus) */
        NCS_NESTED = 1,    /* a recorded G_DL walked in place (WalkNestedList: kept HUD lists) */
        NCS_NIEX = 2,      /* a span program's EX op: the compiler kept the stock handler */
        NCS_NISTX = 3,     /* a span program's ST op whose precondition failed: the stock handlers */
        NCS_NISEG = 4,     /* a span geometry segment the mesh path refused: the stock handlers */
        NCS_NIRECOV = 5,   /* the stock walk after a span program bailed */
        NCS_HUDRUN = 6,    /* a capture run on the HUD replay loop, stock handler */
        NCS_HUDDIRECT = 7, /* a capture run's TEXRECT / LOADBLOCK on the direct emitter (native) */
        NCS_HUDPLAIN = 8,  /* a MENU run the compiler refused / a kept-list run, word by word */
        NCS_HOOKCMD = 9,   /* the G_DL hook re-running a list's own commands (finish mesh) */
        NCS_COURSE = 10,   /* ni_course's leave: the last model's G_VTX re-run */
        NCS_COUNT = 11
    };
    /* level 0 off; every = frames between the compact periodic [nistrict] receipts (0 = final only). */
    void NsSet(int level, uint32_t every);
    /* The tag the current context is attributed to (marker p, 0xC0|cat for an NRR marker, 0xDE for
     * the G_DL hook); returns the previous one (pass it back to restore). */
    int NcTag(int tag);
    /* The cumulative [nistrict] tables; full = every entry line, else the per-bucket summary. */
    void NsDump(const char* why, bool full);
    /* menus-s2 [debug] nicensus_trace=N / nicensus_trace_mode=M / nicensus_trace_skip=K: after K walker
     * commands of game mode M (-1 any), N [nctrace] lines, one per command the walker itself ran (the
     * walkB / walkW residue): the list depth, list base, index and the two words. Measurement only. */
    void NcTraceSet(int lines, int mode, int skip);
    bool NcOn() const {
        return mNc != nullptr;
    }
    /* Opens a rendered frame (one task) in bucket (mode * kNcSubs + sub). */
    void NcFrameBegin(int bucket);
    /* menus-s4 [debug] nicensus_wipe: the open frame drew transition `type` (its bucket's histogram). */
    void NcFrameWipe(int type);
    /* Closes it; the NRR core's per-category markers / draws / verts of the frame (nCats entries). */
    void NcFrameEnd(const uint32_t* nrrMarkers, const uint32_t* nrrDraws, const uint32_t* nrrVerts, int nCats);
    /* A task the owned walker refused (the LUS path drew it). */
    void NcLusTask(int bucket);
    /* Switches the attribution context; returns the previous one (pass it back to restore). */
    int NcSwitch(int ctx);
    /* One interpreted command (after the legacy-F3D remap); site = the Exec loop that ran it. */
    void NcNote(const Cmd& c, bool f3d, int site = NCS_WALK);
    /* Words consumed WITHOUT Exec in the current context (NRR HUD decoded / retained HUD runs). */
    void NcNoteWords(uint32_t words, int retained);
    /* The cumulative per-bucket tables ([nicensus] lines). */
    void NcDump(const char* why, double msPerTick);
    /* NcDump every `every` frames (0 = never). */
    void NcDumpEvery(uint32_t every, double msPerTick);

    /* [nverify] both interpreters share one backend in verify mode; each pass must forget what it
     * believes the backend holds (shader, depth/decal/alpha, bound textures, samplers). */
    void ResetRenderStateCache();
    /* [n60mach] Apply a pending viewport / scissor change to the backend, exactly as the next
     * triangle's ResolveDrawState would (an NRR draw at a marker right after Camera_Draw: the
     * split-screen view's rect must be in place before the NRR draws into it). */
    void ApplyViewportScissor();
    /* niview: a viewport or scissor change waits for the next triangle (ApplyViewportScissor's test). */
    bool ViewportScissorDirty() const {
        return mRdp.viewport_or_scissor_changed;
    }
    /* n60retry: the registers hold a viewport the backend does not have yet. */
    bool ViewportPending() const {
        return mRdp.viewport_or_scissor_changed && memcmp(&mRdp.viewport, &mRs.viewport, sizeof(Rect)) != 0;
    }
    /* [nverify] re-apply this interpreter's cached backend state (the other pass changed it). */
    void ResyncBackend();
    /* [nverify] end-of-task state words, same layout as gdx_gfx_lus_state. */
    unsigned StateSnapshot(uint32_t* out, unsigned cap) const;

    /* --- texture-cache invalidation (gdx_texcache.cpp) ---
     * The owned cache is a second cache beside the LUS one, so the queued requests the game
     * raises (gdx_rdram_mode_reset's arena-rewind clear, minimap.c's per-race address
     * invalidate) must reach it too. Render thread only, like every other cache mutation. */
    void TexCacheClear();
    void TexCacheDeleteAddress(const uint8_t* addr);
    /* native-all S1 [native] atlas_race_reset: retire every entry whose texture is an atlas view
     * (kept or not) and have the backend drop every view and page. Called right before the
     * mode-change TexCacheClear when the new mode is a race. Returns the entries retired. */
    unsigned TexCacheAtlasReset();

    /* N60TEX [debug] tex_keep: a clear keeps the most recently used CONTENT-KEYED, uploaded
     * entries (key.content_hash != 0: the key is a function of the bytes the decode reads, so a
     * later hit is the same decode the clear would have redone) up to `budgetBytes` of backend
     * texture memory, and keeps none while the linear heap is below `floorBytes`. Everything a
     * retire signals (slot generations, the nrr epoch, live bindings) is still signalled for the
     * kept slots. Under linear pressure a miss recycles the oldest kept entry's id first.
     * budgetBytes 0 = the stock clear. */
    void SetTexKeep(uint32_t budgetBytes, uint32_t floorBytes) {
        mTexKeepBytes = budgetBytes;
        mTexKeepFloorBytes = floorBytes;
    }
    struct TexKeepStats {
        uint32_t clears, kept, keptKiB, retired, keptHits, pressureRetires, linFreeKiB;
    };
    const TexKeepStats& TexKeepReceipt() const {
        return mTexKeep;
    }

    /* [native] texhash_ci: fold the decoded source span (and, for CI, the live palette bytes)
     * into the key for the CI/RGBA32 classes. 0 restores the identity-only key. */
    void SetCiContentHash(bool on) {
        mHashCiRgba32 = on;
    }

    /* Forcing knobs for DROPSITE 1, geomloss's vbopoolkib for this site. Both default to 0 =
     * shipped behaviour.
     *   [debug] texscratchkib=N    -- start the decode scratch at N KiB instead of 256, so an
     *                                 ordinary texture exercises the GROWTH path.
     *   [debug] texscratchmaxkib=N -- cap the growth ceiling at N KiB, so an ordinary texture
     *                                 exercises the OVERFLOW + neutral-fallback path. Setting
     *                                 both to the same N reproduces a pre-fix hard-ceiling
     *                                 scratch of exactly that size (256 = the shipped pre-fix). */
    void SetTexScratchKib(uint32_t startKib, uint32_t maxKib) {
        SetTexScratchBytes(startKib * 1024u, maxKib * 1024u);
    }
    /* The same knob in bytes, so the host tests can straddle an 8x8 decode (256 B) without
     * building a megabyte display list. */
    void SetTexScratchBytes(uint32_t startBytes, uint32_t maxBytes) {
        mTexUploadMax = maxBytes;
        if (startBytes == 0) {
            return;
        }
        const uint32_t want = startBytes;
        if (want == mTexUploadCap) {
            return;
        }
        uint8_t* p = new (std::nothrow) uint8_t[want];
        if (p == nullptr) {
            return;
        }
        delete[] mTexUpload;
        mTexUpload = p;
        mTexUploadCap = want;
    }
    /* [debug] vtxmax -- shrink the effective G_VTX window below kMaxVertices. Test knob only;
     * the storage is unchanged, so nothing can overrun. 0 = shipped (kMaxVertices). */
    void SetVtxSoftCap(uint32_t n) {
        mVtxSoftCap = (n == 0 || n > static_cast<uint32_t>(kMaxVertices)) ? static_cast<uint32_t>(kMaxVertices) : n;
    }
    /* [native] vtxclamp -- 0 restores the pre-fix behaviour: an over-range G_VTX loads nothing
     * at all. A/B only. */
    void SetVtxClamp(bool on) {
        mVtxClamp = on;
    }
    /* [debug] clippolymax -- shrink the clip-polygon cap below 16 to force the truncation arm.
     * 0 = shipped. Never raised above 16: that is the buffer's real size. */
    void SetClipPolyMax(uint32_t n) {
        mClipPolyMax = (n < 3u || n > 16u) ? 16u : n;
    }
    /* [native] texstalefix -- 0 restores the pre-fix behaviour: an import that uploaded nothing
     * is still marked uploaded, so the id keeps the previous texture's pixels. A/B only. */
    void SetTexStaleFix(bool on) {
        mTexStaleFix = on;
    }

    /* DROPSITES: session totals for the drop counters, which the windowed `Counters` cannot
     * carry. Measured: a forced growth run put `texgrow>0` in 2 of 91 receipt windows -- a
     * per-window counter that reads 0 in the other 89 is how `texDecodeOverflow` stayed
     * invisible in the first place, so every drop also gets a number that never resets. */
    struct SessionDrops {
        uint32_t texOvf, texGrow, texNoUp, texFb, vtxOver, vtxDropped, clipTrunc;
    };
    SessionDrops Drops() const {
        SessionDrops d = mSesDrops;
        d.texOvf += mStats.texDecodeOverflow;
        d.texGrow += mStats.texDecodeGrow;
        d.texNoUp += mStats.texNoUpload;
        d.texFb += mStats.texFallback;
        d.vtxOver += mStats.vtxOver;
        d.vtxDropped += mStats.vtxOverDropped;
        d.clipTrunc += mStats.clipTrunc;
        return d;
    }

    uint32_t TexDecodeHwBytes() const {
        return mTexDecodeHwBytes;
    }
    uint32_t ClipPolyHw() const {
        return mClipPolyHw;
    }
    uint32_t VtxHwEnd() const {
        return mVtxHwEnd;
    }
    uint32_t TexScratchCap() const {
        return mTexUploadCap;
    }
    unsigned TexLive() const {
        return mTexCount;
    }
    unsigned TexLiveHw() const {
        return mTexCountHw;
    }

    /* Public for the emission helpers in gdx_tri.cpp. */
    void Flush(FlushReason why);
    /* [native] replay (C6, gdx3ds_replay.h): the matrix-state tag of the open batch; a triangle
     * with a different tag drains it first so every draw has exactly one pose. */
    uint16_t mReplayBatchTag = 0;
    uint16_t mReplayClipTag = 0; /* the tag of the triangle the CPU clipper is fanning */
    void ReplayBatchTag(uint16_t tag);
    uint16_t ReplayTriTag(uint8_t a, uint8_t b, uint8_t c, bool isRect) const;
    float ReplayVtxXscale() const;
    /* The vertex-load half of the hooks: source record + per-slot tag for n slots at dest (every
     * path that fills loaded_vertices from a G_VTX, including the sky's memoised loads). */
    void ReplayNoteVertexLoad(const uint8_t* src, size_t n, size_t dest, bool isBig);
    float ReplayRectXscale() const;
    LoadedTexture LoadedTexAt(uint16_t word) const;

    /* --- [native] machines_texskip (gdx_texskip.cpp; C5 Stage 1, c5s1-progress.md) ---
     * Racer_Draw emits, per drawn machine, `G_DL D_800CDD38[slot]` (the machine's texture-set
     * list: 8 x SETTIMG/SETTILE/LOADBLOCK/SETTILESIZE) + SETENVCOLOR + `G_DL` body. The body
     * is drawn natively (gdx3ds_mesh.cpp); the texture-set list is still walked. With the
     * lever on, a texture-set list is SKIPPED when (a) its slot has a record of what the list
     * leaves behind (tile descriptor fields, TMEM span views), (b) the body that follows is a
     * native machine mesh whose tiles are immutable (o2r asset) sources, and (c) the lists that
     * really RUN later in the same command group rewrite every TMEM word and tile field it
     * writes, and no later body samples one of its tiles as a tile its own list leaves alone --
     * so the state after the group is byte-for-byte what the stock stream leaves. Between the
     * skip and the next list (the "window") the recorded state is served through LoadedTexAt
     * and the swapped tile descriptors; immutable sources are keyed and decoded from their
     * SOURCE bytes, never TMEM, so the imports inside a window are the stock imports. The
     * tripwire counts any texture-state writer, non-immutable import or unexpected G_DL inside
     * a window. gdx_texskip.cpp carries the full argument.
     * Levels: 0 off (default), 1 skip, 2 verify (never skips; compares the record with the
     * live state wherever a skip would have happened). OwnedDlHook (gdx_rt_glue.cpp) calls
     * TexSkipOnDl before the machines hook and TexSkipAfterDl after it. */
    /* The machine-registry queries the skip needs, injected by the glue (gdx3ds_mesh.cpp via
     * gdx_rt_glue.cpp) so this file stays host-compilable. */
    struct TexSkipHooks {
        int (*setSlot)(const void* list);   /* slot whose texture-set list this is, else -1 */
        int (*isSpeck)(const void* list);   /* 1 = the shared LOD-6 far speck */
        int (*isBody)(const void* list);    /* 1 = a registered machine body list (no bake) */
        /* 1 = `body` is a machine list the G_DL hook will draw natively; its slot and the
         * tiles its runs sample, given the tile / combiner texel use it inherits (inhUse bit0 =
         * cycle-1 TEXEL0, bit1 = cycle-1 TEXEL1, bit2 = any texel in cycle 2). */
        int (*bodyProbe)(void* user, const void* body, uint8_t inhTile, uint8_t inhUse, int* slot, uint8_t* tiles);
        uint32_t (*generation)(void);       /* machine-list generation */
        void* user;
    };
    void SetTexSkip(int level, const TexSkipHooks& hooks);
    int TexSkipOnDl(const void* listPtr); /* 1 = consumed (a skipped set) */
    void TexSkipAfterDl(const void* listPtr);
    void TexSkipTaskEnd();
    struct TexSkipCounters {
        uint32_t lists, skips, real, recs, noRec, noBody, noTiles, noNext, noCover, unsup;
        uint32_t vfyChecks, vfyTileMis, vfyLtMis, vfyWould;
        uint32_t trips, tripWriter, tripImport, tripDl, tripTaskEnd;
        uint32_t ovImports, plans;
    };
    const TexSkipCounters& TexSkipStats() const {
        return mTexSkipStats;
    }
    void TexSkipResetStats() {
        memset(&mTexSkipStats, 0, sizeof(mTexSkipStats));
    }
    int TexSkipLevel() const {
        return mTexSkip;
    }

    /* --- [debug] machsplit (gdx_machsplit.cpp; c5mach2-progress.md) ---
     * Diagnostic split of the [c5cat] MACH category: inside the machine section of Racer_Draw
     * every command at the section's own list depth opens a class (matrix, texture-set list
     * walked / skipped, body, highlight body, far speck, other list, state) and its gross time,
     * including the lists it pushes, is charged to that class; the native mesh draw's own time
     * (meshTicks) is subtracted, so the classes add up to `mach`. One clock read per section
     * command, a separate walk loop (the shipping loop carries no branch). Off by default. */
    void MachSplitSet(int on, uint64_t (*meshTicks)(void));
    /* [native] machines_loadmemo (gdx_setload.cpp; c5mach2): 0 off, 1 the machine texture-set
     * lists' LOADBLOCKs served from their memoised resolved form, 2 verify. */
    void SetLoadMemo(int level);
    void LoadMemoTaskEnd();
    /* [native] dlmemo (gdx_dlmemo.cpp; c5dlmemo-progress.md §1): 0 off, 1 the G_DL resolve
     * served from a cross-task memo, 2 verify (the resolve always runs and is compared). */
    void SetDlMemo(int level);
    /* [native] deadsetup (gdx_dlmemo.cpp; c5dlmemo-progress.md §2): 0 off, 1 the state
     * commands of back-to-back setup lists whose output no later command can observe are not
     * executed (a list with none left is not entered), 2 verify. Needs dlmemo. */
    void SetDeadSetup(int level);
    /* One receipt line each per `tasks` tasks ([dlmemo], [deadset]); 0 when there is nothing. */
    int DlMemoFormat(char* buf, size_t cap, uint32_t tasks);
    int DeadSetFormat(char* buf, size_t cap, uint32_t tasks, double msPerTick);
    /* Session totals (host tests): plans, lists skipped, dead commands, verify ok, verify bad,
     * trips, plans reused from the cache; and the memo's hits. */
    void DeadSetSession(uint32_t out[7]) const;
    uint32_t DlMemoSessionHits() const;
    /* [native] machines_speck (c5mach2): the G_DL being dispatched names the shared LOD-6 far
     * speck (its placeholder operand; the walker resolves it to the loaded asset image). */
    bool DlOperandIsSpeck(const void* listPtr) {
        return mTexHooks.isSpeck != nullptr && TexSkipDlIsSpeck(listPtr);
    }
    /* The resolved list the G_DL hook is being offered (valid inside the hook call only). */
    const ListRef& DlHookList() const {
        return mDlHookSub;
    }
    /* nicourse [native] ni_course (gdx_nicourse.cpp): the course preview's G_NOOP p=0x6B marker. The
     * model's G_VTX / G_TRI2 run right after the marker pair is drawn from a GPU-resident copy and the
     * walker moves past it, or the run is left to the walk. `seam` = the owned GdxInterpSeam. */
    void NrrCourseHotMarker(uint32_t w0, uint32_t w1, void* seam);
    void MachSplitTaskEnd();

    /* --- [native] hud_batch (gdx_hud_replay.cpp; C5 Stage 2, c5s2-progress.md) ---
     * The game-side capture (gdx3ds_hud_capture.c) moved runs of RDP-state / texture-load /
     * rectangle commands out of the HUD's display list into a record and left one G_NOOP
     * p=0x5E marker per run; the glue's noop hook calls HudReplayRun at that marker, which
     * executes the run's commands through Exec -- the walker's own handlers, same order, same
     * position. Two memos ride on it, both only while a run replays (mHud != nullptr) and both
     * only for `immutable` (o2r asset) sources: the SETTIMG source resolution
     * (HudResolveTextureSource) and the immutable content hash of ImportTexture's key
     * (HudHashLookup/Store). Level 2 (verify) computes the stock answer every time and
     * compares it with the memo instead of using it. HudMemoClear follows the texture cache's
     * clear (gdx_rdram_mode_reset). */
    void HudReplayRun(const void* record, uint32_t runIndex);
    void HudReplayRunOne(void* record, uint32_t runIndex); /* n60results: one run of a marker group (a GdxHudRecord) */
    void HudMemoClear();
    /* [hudop] per-opcode self time of the replayed runs (gputrace only); resets the window. */
    int HudCensusFormat(char* buf, size_t cap, uint32_t tasks, double msPerTick);
    /* [native] hudcensus=1 (with gputrace): arm the [hudop] clock (two tick reads per command). */
    void HudSetCensus(bool on) {
        mHudCensus = on;
    }

    /* --- [native] hud_direct (gdx_hud_direct.cpp; C5 Stage 2b, c5s2b-progress.md) ---
     * 0 off, 1 on, 2 verify. Inside a replayed HUD run: LOADBLOCKs leave their TMEM bytes as a
     * DEBT (source + generation stamp) instead of copying them, the texture key's TMEM hash comes
     * from a per-source memo, and TEXRECTs are emitted straight into the packed batch. Every
     * reader of TMEM bytes pays the debt first (MirrorToTmem trims it, ImportTexture hashes or
     * pays it, the task end pays the rest). Level 2 runs the stock path and cross-checks. */
    void HudSetDirect(int level) {
        mHudDirect = level;
    }
    /* [native] hudclock: 1 = a clock pair around every direct run, 2 = also around every
     * direct rectangle and load ([hudd] runms/rectms/loadms; diagnostic, inflates by its reads). */
    void HudSetClock(int level) {
        mHudClock = level;
    }
    void HudDirectTaskEnd();
    void HudDirectClear();
    int HudDirectFormat(char* buf, size_t cap, uint32_t tasks);

    /* --- [native] hudelem / hud_retain (gdx_hud_retain.cpp; c5hud3-progress.md) ---
     * hudelem=1: the per-run HUD census ([hudelem] lines once every 512 tasks, [hudelem] sum on
     * the receipt cadence). Diagnostic; changes nothing drawn. */
    void HudSetElem(int level) {
        mHudElem = level;
    }
    /* [native] hud_retain: 0 off, 1 on, 2 verify (gdx_hud_retain.cpp). Inside a direct HUD run
     * (hud_batch + hud_direct=1): a run whose words, the entry registers it reads, its texture
     * sources and cache slots and the backend facts it depends on all match the recorded run
     * re-submits the recorded backend calls and vertex floats and applies the recorded state
     * effects instead of executing. Level 2 executes every would-be hit and compares. */
    void HudSetRetain(int level, int clock) {
        mHudRetain = level;
        mHudRetClock = clock; /* [native] hudretclock: retention's own time split (diagnostic) */
    }
    void HudRetainTaskEnd();
    void HudRetainClear();
    int HudRetainFormat(char* buf, size_t cap, uint32_t tasks);
    /* [native] hudsky_emit: 0 off, 1 on, 2 verify (gdx_hudsky_emit.inc, hudsky-progress.md). With
     * hud_retain=1, a retained HUD run's hit is decided by a compiled fingerprint and replayed as
     * ONE backend program (Backend::HsxRun) with a compiled register write-back; the batches a
     * sky run drains take the backend's lean packed-draw tail (Backend::DrawPackedLean). */
    void HsxSetLevel(int level) {
        mHudSkyEmit = level;
    }
    int HsxFormat(char* buf, size_t cap, uint32_t tasks);
    /* --- [native] nrr_hud (gdx_nrr_hud.cpp; n60hud-progress.md, n60-design.md §4.1) ---
     * The NRR HUD: a HUD-kind 0x5E run is decoded by one small loop (register ops written to the
     * live registers, loads kept virtual, rectangles emitted from memoised state blocks) instead
     * of the replay. 0 off, 1 on, 2 verify (the run executes stock; the decoder's state blocks,
     * floats and register write-back are compared with it). census: [nrrhud] inventory lines. */
    void NrrHudSet(int level, int census, int clock, int wide) {
        mNrrHud = level;
        mNrrCensus = census;
        mNrrClock = clock;
        mNrrWide = wide;
    }
    /* [native] nrr_hud_memo (n60gp): a program's rectangle whose every input (its words, block,
     * corner colours, the corner mapping) is the one it had last time copies the six vertices it
     * produced then instead of computing them again (bit-identical by construction). 0 off. */
    void NrrHudMemoSet(int level) {
        mNrrHudMemo = level;
    }
    void NrrHudDepDiagSet(int n) {
        mNrrDepDiag = n;
    }
    void NrrHudLateSet(int bits) {
        mNrrLate = bits;
    }
    /* [native] nrr_hud_kept (n60hudx, default 0): the HUD capture records the G_DLs to the HUD's
     * static setup lists inside the runs (gdx3ds_hud_capture.c); the NRR HUD compiles each into its
     * run's program (an SL op: the list's register words as masked stores, re-checked against the
     * resolved list every time); any other path walks it nested at its place (HudNestedDl). */
    void NrrHudKeptSet(int level) {
        mNrrKept = level;
    }
    /* [debug] nrr_hud_runclock=1 (n60hudx, diagnostic): a clock pair around every NRR HUD run, kept per
     * run position in the task ([nrrrun] line beside [nrrhud]). */
    void NrrHudRunClockSet(int on) {
        mNrrRunClock = on;
    }
    /* [native] nrr_hud_rr (n60hudx, default 0): an unchanged run replays its last output -- the
     * program's register and load ops run (merged), every rectangle's six vertices come from the
     * record of the run's last execution (gdx_nrr_hud.cpp "run replay"). Bits: 1 runs of >= 8
     * rectangles (the race-position portrait column), 2 every run, 4 verify (the run executes, and
     * its output is compared with the record). */
    void NrrHudRrSet(int bits);
    /* nimenu [native] ni_menu (gdx_nrr_menu.inc): 0 off, 1 the MENU-kind runs (non-race modes' draw,
     * gdxmenu_capture in gdx3ds_hud_capture.c) through the NRR program runner, 2 verify. Needs the
     * NRR HUD (nrr_hud=1). */
    void NiMenuSet(int level) {
        mNiMenu = level;
    }
    int NiMenuLevel() const {
        return mNiMenu;
    }
    /* menus-s2 [debug] nimenu_voldiag=N: up to N lines naming the words a decoded MENU run changed
     * since the same run ordinal's previous decode (diagnostic; allocates a copy store on first use). */
    void NiMenuVolDiagSet(int lines) {
        mNiVolDiag = lines > 0 ? lines : 0;
    }
    /* menus-s2b [native] ni_menu_split (default 0), bits: a MENU run with no program runs as segments,
     * each through its own program (gdx_nrr_menu.inc NrrMenuSplit) -- 1 its FILLRECT chains (the tile
     * wipe: a rectangle count that changes every frame) in power-of-two pieces, 2 when it took the
     * decoder in the previous task too, one segment per texture load (the tile / strip transitions). */
    void NiMenuSplitSet(int bits) {
        mNiMenuSplit = static_cast<uint8_t>(bits & 3);
    }
    /* menus-s2b [native] ni_menu_keep=1 (default 0): a MENU run that was decoded in the previous task too
     * (its ordinal's mNiMenuVol is non-zero: a transition's moving tiles, a run whose words change every
     * frame) may not empty the NRR HUD's block memo or program pool when it finds them full -- its
     * rectangle stays the stock one it just drew, its program is not stored. Every other run clears as
     * before (gdx_nrr_hud.cpp NrrCaptureAfter / NrrProgStore). */
    void NiMenuKeepSet(int on) {
        mNiMenuKeep = on == 1;
    }
    int NiMenuFormat(char* buf, size_t cap, uint32_t tasks, int gameMode);
    void NrrRunClockNote(uint32_t runInTask, const void* runWords, uint32_t n, uint64_t ticks);
    int NrrHudFormat(char* buf, size_t cap, uint32_t tasks);
    void NrrHudTaskEnd();
    void NrrHudClear();
    int HudElemFormat(char* buf, size_t cap, uint32_t tasks);
    struct HudRegs;   /* gdx_hud_retain.cpp: the register view a HUD run reads and writes */
    struct HudRetRec; /* gdx_hud_retain.cpp: one retained run */
    /* --- C5 Stage 3b direct-load scopes (gdx_hud_direct.cpp; port/3ds/gfx/gdx3ds_dress.h) ---
     * Inside a scope opened by a decomp p=0x6C marker (the gadget pass, the decoration passes,
     * the effects section) every walker G_LOADBLOCK runs hud_direct's load: the resolved form
     * from the memo, the TMEM bytes left as debt (settled by the next reader or the task end).
     * [native] fxshd_direct also routes the LOADBLOCKs of the shadow / boost-flame replay runs
     * here. `level` per scope kind: 0 off, 1 on, 2 verify (the stock load runs and is compared). */
    void DirectSetLevels(const int levels[4]) {
        for (int k = 0; k < 4; k++) {
            mDirectLevels[k] = levels[k];
        }
    }
    void DirectScope(int kind) {
        mDirectScope = (kind > 0 && kind < 4 && mDirectLevels[kind] != 0) ? kind : 0;
    }
    int DirectScopeKind() const {
        return mDirectScope;
    }
    int DirectLevel(int kind) const {
        return (kind >= 0 && kind < 4) ? mDirectLevels[kind] : 0;
    }
    int DirectFormat(char* buf, size_t cap, uint32_t tasks);
    /* --- [native] sky_retain / skyclock (gdx_sky_retain.cpp; c5sky-progress.md) ---
     * The p=0x65 markers Background_Draw writes (gdx3ds_sky_capture.c). SkyMarker takes each
     * one where the walker meets it; SkyFormat writes the receipts line by line (index 0..),
     * returning 0 past the last; SkyClear follows the texture cache's clear. */
    void SkyMarker(uint32_t w0, uint32_t w1);
    /* The census reads the [prof] section ticks/calls (GDX3DS_PROF_*) through this; null = none. */
    static constexpr int kSkyProfSections = 7;
    typedef void (*SkyProfFn)(uint64_t ticks[kSkyProfSections], uint32_t calls[kSkyProfSections]);
    void SkySetLevels(int retain, int clock, SkyProfFn prof);
    int SkyFormat(char* buf, size_t cap, int line);
    void SkyTaskEnd();
    void SkyClear();
#ifdef EXPANSION_KIT
    /* [native] ek_editmesh (gdx_ekpreview.cpp, o3dsek plan S5c): the course editor's preview, one
     * output memo per element (ground-grid quad, course line). Per task: the receipt and the idle free. */
    void EkPvTaskEnd();
#endif
    /* --- [native] nrr_sky (gdx_nrr_sky.cpp; n60sky-progress.md): the NRR sky category. The game
     * side's p=0x64 LEARN markers bank the stock state blocks here (cold); the core's 0x63 marker
     * draws (NrrSkyHotDraw, a free function) read the walker through the NrrSkyHot* members. */
    void NrrSkyMarker(uint32_t w0, uint32_t w1);
    int NrrSkyFormat(char* buf, size_t cap, uint32_t tasks);
    bool NrrSkyHotTex(int16_t slot, uint32_t id, const uint8_t* addr, uint32_t contentHash);
    void NrrSkyHotHead(float mp[4][4], float* xs, int16_t scissor[4]) const;
    void NrrSkyHotProj(float out[4][4]) const;
    /* --- [native] nrr_shd / nrr_fx (gdx_nrr_fx.cpp; n60fx-progress.md): shadows, boost flames and
     * effects on the NRR. The game side's p=0x66 LEARN markers bank stock state blocks here (cold);
     * the core's 0x63 markers draw (free functions) and read the walker through NrrFxHot*. */
    void NrrFxMarker(uint32_t w0, uint32_t w1);
    int NrrFxFormat(char* buf, size_t cap, uint32_t tasks);
    bool NrrFxHotTex(int16_t slot, uint32_t id, const uint8_t* addr);
    void NrrFxHotHead(float mp[4][4], float xs[2], int16_t scissor[4]) const;
    const void* NrrFxResolveVtx(const void* src, uint32_t n); /* cold: the walker's own G_VTX resolution */
    void NrrFxHotFog(int16_t* mul, int16_t* off, float rgb[3]) const;
    /* --- nimach (nointerp S5b; nimach-progress.md): a menu machine's gSPViewport. Computes the
     * pending viewport/scissor only (CalcAndSetViewport); the caller (gdx3ds_nrr_mach.cpp
     * NrrMachMenuHotEnt) applies it through the same NrrEnd/ApplyViewportScissor/NrrBegin cycle
     * as niview's NrrFxColdMultiView (gdx_nrr_fx.cpp) -- one idiom for an in-hand-off viewport
     * change instead of two (native-review-2026-10-15.md Sec.2 "in-hand-off viewport change"). */
    void NrrMachViewport(const int16_t* vp) {
        CalcAndSetViewport(vp);
    }
    /* nimach: the fog colour's alpha (a blend-colour run's fog factor, XfFillRun's fogFactorConst). */
    void NrrMachFogAlpha(uint8_t* fogA) const {
        *fogA = mRdp.fog_color.a;
    }
    void NrrMachViewportDirty() {
        memset(&mRs.viewport, 0, sizeof(mRs.viewport));
        memset(&mRs.scissor, 0, sizeof(mRs.scissor));
        mRdp.viewport_or_scissor_changed = true;
    }

  private:
    void NrrFxLearnBegin(uint32_t cat, uint32_t kind);
    void NrrFxLearnEnd(uint32_t cat, uint32_t kind);
    bool NrrFxProbe(void* block);
    void NrrSkyLearnBegin(uint32_t kind);
    void NrrSkyLearnEnd(uint32_t kind, uint32_t gen);
    bool NrrSkyProbe(void* block);
    bool NrrSkyLearnStars();
    struct SkyState; /* gdx_sky_retain.cpp */
    struct SkyGroup;
    SkyState* mSky = nullptr;
    int mSkyRetain = 0;
    int mSkyClock = 0;
    SkyProfFn mSkyProf = nullptr;
    bool mSkyPending = false; /* deferred texture-state groups are queued (SpTri materializes) */
    bool mSkyActive = false;  /* a sky run is replaying (ImportTexture's hash memo hooks) */
    bool mSkyRectFast = false; /* this draw follows a draw of its kind with only colour writes between */
    bool mSkyNoClip = false;   /* sky_retain=3 (NOT exact): the sky's triangles skip the CPU clipper */
    bool SkyRectState(bool isRect, DrawState& out);
    bool SkyEnsure();
    void SkyCensusSwitch(int section);
    int SkyFormatCensus(char* buf, size_t cap, int line);
    int SkyFormatReceipt(char* buf, size_t cap);
    int SkyFormatOps(char* buf, size_t cap);
    bool SkyRunValid(const void* record, uint32_t runIndex) const;
    void SkyReplayRun(const void* record, uint32_t runIndex);
    void SkyReplayWords(uint16_t start, uint16_t end, const ListRef& hint);
    bool SkyGroupEliminable(uint16_t start, uint16_t end) const;
    bool SkyGroupsMatch(const SkyGroup& a, uint16_t start, uint16_t end) const;
    void SkyGroupOpen(int wordIndex);
    void SkyGroupClose();
    void SkyExecWords(uint16_t start, uint16_t end);
    bool SkyGroupCovered(const SkyGroup& g, uint32_t unresolvedBefore) const;
    void SkyExecGroup(const SkyGroup& g);
    void SkyMaterialize();
    bool SkyVerifyEnsure();
    void SkyVerifySnapshotS0();
    void SkyVerifyMaterialize();
    bool SkySrcHashLookup(const uint8_t* addr, uint32_t n, bool immutable, uint32_t* out);
    void SkySrcHashStore(const uint8_t* addr, uint32_t n, bool immutable, uint32_t h);
    bool SkyPalHashLookup(const uint8_t* bytes, uint32_t n, uint32_t* out);
    void SkyPalHashStore(const uint8_t* bytes, uint32_t n, uint32_t h);
    bool SkyTmemSpanMirrorsSource(uint16_t base, uint32_t span) const;
    bool SkyTmemHashLookup(uint16_t base, uint32_t span, uint32_t* out);
    void SkyTmemHashStore(uint16_t base, uint32_t span, uint32_t h);
    void SkyVertex(const Cmd& c, const ListRef& hint);
    struct HudDirect; /* gdx_hud_direct.cpp */
    HudDirect* mHudD = nullptr;
    int mHudDirect = 0;
    int mHudClock = 0;
    void HudDirectTicks(int what, uint64_t ticks);
    bool mHudVirtualLoad = false; /* the LOADBLOCK being executed leaves its bytes as TMEM debt */
    uint32_t mHudDebtN = 0;       /* live debt entries (or shadow entries under verify) */
    bool HudDirectEnsure();
    bool HudDirectBegin(); /* a direct run starts: the receipt, and whether it is a verify run */
    bool HudDebtMirror(uint8_t* dst, const uint8_t* src, uint32_t n, bool nativeRgba16);
    bool HudDebtHash(uint32_t off, uint32_t span, uint32_t* out);
    void HudDebtPayAll();
    struct Frame {
        ListRef list;
        size_t idx;
    };
    void SkyExecTextureOp(const Cmd& c, Frame& f); /* [native] sky_retain (gdx_sky_retain.cpp) */
    void SkyReplayWord(const Cmd& c, Frame& f, Frame& hintFrame, uint8_t op, int at);
    /* [native] hud_direct: a TEXRECT (+ its two RDPHALF trailers) of a direct HUD run, and the
     * pieces it shares with its verify mode (gdx_hud_direct.cpp). */
    void HudDirectTexRect(const Cmd& c, Frame& f);
    void HudVerifyTexRect(const Cmd& c, Frame& f);
    void HudDirectLoadBlock(const Cmd& c, Frame& f);
    /* C5 Stage 3b: a LOADBLOCK served by the direct path for scope `kind` (0 = the shadow / flame
     * replay runs under fxshd_direct; 1..3 = GDXDRESS_SCOPE_*). */
    void DirectScopeLoadBlock(const Cmd& c, Frame& f, int kind, int level);
    int mDirectScope = 0;
    int mDirectLevels[4] = { 0, 0, 0, 0 };
    bool mDirectInLoad = false;
    struct DirectWin {
        uint32_t hit[4], miss[4], stock[4], vfyOk, vfyBad;
    } mDirectWin = {};
    bool HudRectGeometry(const Cmd& c, const Cmd& h1, const Cmd& h2, LoadedVertex v[4], float* lrs, float* lrt) const;
    void HudRectPositions(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, LoadedVertex v[4]) const;
    bool HudTrectEligible(uint8_t tile, int16_t uls, int16_t ult, float lrs, float lrt) const;
    bool HudRetainVariant(HudRetRec& rec, const void* runWords, uint32_t n);
    void HudRetainPackRect(const void* rectCtx, const LoadedVertex quad[4], float* corners, Gdx3dsVboPackAux* aux);
    bool HudDirectTri(const int idx[3], DrawState& ds, bool haveState, float* corners, bool* packed);
    void HudPackVerts(const DrawState& ds, const LoadedVertex* const* v, int n, const LoadedVertex* v1,
                      float* out, bool halfTexel = false) const;
    void HudPackAux(const DrawState& ds, const LoadedVertex* v0, Gdx3dsVboPackAux* aux) const;

    struct HudRetain; /* gdx_hud_retain.cpp */
    class HudRecBackend; /* gdx_hud_retain.cpp: the recording decorator over mBackend */
    HudRetain* mHudR = nullptr;
    int mHudElem = 0;
    int mHudRetain = 0;
    int mHudRetClock = 0;
    int mHudSkyEmit = 0; /* [native] hudsky_emit */
    bool mHsxLean = false; /* a HUD or sky run replays under hudsky_emit=1: its drains take DrawPackedLean */
    /* [native] hudsky_emit, the sky half (gdx_hudsky_tri.inc): a sky run's triangles through the
     * compact SpTri (level 1), or SpTri with every emitted triangle's floats checked against the
     * compact packing (level 2). */
    bool mHsxSky = false;
    bool mHsxSkyVfy = false;
    uint32_t mHsxSkyTris = 0, mHsxSkyCalls = 0, mHsxSkyStock = 0;
    uint32_t mHsxSkyVfyOk = 0, mHsxSkyVfyBad = 0, mHsxSkyVfyBadSes = 0;
    bool HsxSkyOk() const;
    void HsxSkyTri(uint8_t a, uint8_t b, uint8_t c);
    void HsxSkyTail(LoadedVertex* v1, LoadedVertex* v2, LoadedVertex* v3, uint16_t replayTag);
    void HsxSkyVerifyEmit(const DrawState& ds, const LoadedVertex* const* v, const float* out, bool halfTexel);
    struct HsxStats;
    HsxStats* mHsx = nullptr;
    bool HsxEnsure();
    int HsxGroupFields(int group, void* out) const;
    bool HsxCompile(HudRetRec& rec);
    bool HsxPlanReady(HudRetRec& rec);
    bool HsxFingerprint(HudRetRec& rec, int* why);
    int HsxExactFingerprint(HudRetRec& rec, const void* runWords, uint32_t n, int* why, bool* hit);
    void HsxApply(const HudRetRec& rec);
    void HsxRunGeneric(Gdx3dsHsxProgram* prog);
    void HsxReplay(HudRetRec& rec);
    void HsxVerifyPlan(HudRetRec& rec, bool stockHit, bool hsxHit);
    void HsxVerifyApply(const HudRetRec& rec);
    void HsxNoteRun(bool retained, uint64_t ticks);
    void HsxMark(int phase) {
        if (mHudClock >= 2) {
            HsxMarkSlow(phase); /* hudclock=2 only: the emitter's hit phase clock */
        }
    }
    void HsxMarkSlow(int phase);
    void HsxPhaseEnd(bool retained);
    bool mHudRec = false; /* a retained run is being recorded (the hooks below note into it) */
    bool mHudLastTrectOk = false; /* the last direct TEXRECT's [trectbatch] eligibility */
    bool HudRetainEnsure();
    bool HudElemCensusTask() const;
    void HudElemBegin();
    void HudElemEnd(const void* runWords, uint32_t n);
    /* The run's entry: 1 = handled (a hit was replayed), 0 = execute it (maybe recording). */
    int HudRetainBegin(const void* runWords, uint32_t n);
    void HudRetainEnd();
    void HudRecOpPre(uint32_t wordIndex, uint8_t op);
    void HudRecOpPost(uint32_t wordIndex, uint8_t op);
    void HudRecNoteLoad();
    void HudRecNoteRect(uint32_t wordIndex, uint8_t op);
    void HudRecNoteImportBegin(uint16_t tmemIdx, uint32_t hashOff, uint32_t hashSpan, bool tmemHashed, bool covered,
                               bool supported);
    void HudRecNoteImportHit(int unit, int16_t slot, bool uploaded);
    void HudRegCapture(HudRegs& r) const;
    void HudRegCaptureGroup(HudRegs& r, int group) const;
    bool HudRegSame(const HudRegs& a, const HudRegs& b, int group) const;
    bool HudRegSameLive(const HudRegs& b, int group) const;
    bool HudColourSame(uint8_t op, uint32_t w0, uint32_t w1) const;
    void HudRegApply(const HudRegs& r, int group);
    bool HudRetainFingerprint(const HudRetRec& rec, const void* runWords, uint32_t n, bool checkWords, int* why);
    void HudRetainReplay(HudRetRec& rec);
    bool HudRetainSameContentPlan(const HudRetRec& rec) const;
    void HudRetainVerifyPrepare(HudRetRec& rec);
    void HudRetainVerifyCompare(const HudRetRec& rec);
    void HudRetainRunDone();
    void TexCacheTouch(int16_t slot);
    /* gdx_hud_direct.cpp: a direct load's resolved form, applied without resolving (the hit
     * path's tail), and the debt ledger as an opaque snapshot (retention verify). */
    struct HudLoadForm {
        LoadedTexture loaded;
        const uint8_t* tlAddr;
        uint32_t copyBytes;
        uint16_t tmemIndex;
        uint8_t cls, native, imm, valid;
        uint8_t skipped; /* an immutable load the same-content test skipped (nothing stored) */
    };
    const HudLoadForm* HudDirectLastLoad() const;
    void HudDirectApplyLoad(const HudLoadForm& form, bool countLoad);
    void HudDirectApplySpan(const HudLoadForm& form, const uint64_t srcStamp[2], unsigned srcCls);
    bool HudDirectLastHashCovered() const;
    void HudDirectLastHashSource(const uint8_t** src, uint8_t* native, uint8_t* cls) const;
    bool HudDebtCoverSame(uint32_t off, uint32_t span, const uint8_t* src, uint8_t native, uint8_t cls) const;
    size_t HudDebtStateBytes() const;
    void HudDebtStateSave(void* out) const;
    void HudDebtStateLoad(const void* in);
    void HudDebtMaterialize(uint8_t* out4096) const;

    /* [native] nrr_hud (gdx_nrr_hud.cpp; n60hud-progress.md). The decoder keeps the run's
     * register writes LIVE (the interpreter's own setters) and its LOADBLOCKs VIRTUAL: a load
     * records which source now sits at which TMEM words (the texture identity a rectangle's
     * state block is keyed on) and is queued as PENDING; its TMEM words are STALE in the
     * interpreter until an interpreter reader needs them (NrrTmemRead materialises the pending
     * loads, cold) or an interpreter writer overwrites them (NrrTmemWrite drops what it covers
     * and materialises what it only partly covers first, so the write order stays the stock one).
     * mNrrTmemLive gates both hooks: false = the NRR holds no TMEM state, the hooks cost a test. */
    struct NrrHud;
    struct NrrBlock;
    struct NrrKey;
    struct NrrRegFx;
    struct NrrProg;
  public:
    /* HudPackVerts' unit-0 texture coordinate constants, per axis (u, v) (gdx_nrr_hud.cpp) */
    struct NrrBlockUv {
        float pre[2], off[2], normMul[2], normDiv[2], scale[2], clamp[2], viewOff[2];
        uint8_t mul[2];
        uint8_t hasOff;
    };

  private:
    NrrHud* mNrr = nullptr;
    int mNrrHud = 0;
    int mNrrCensus = 0;
    int mNrrClock = 0;
    int mNrrLate = 0;    /* [native] nrr_hud_late (n60intro): 1 late-bound scissor, 2 wide-load pitch checked at run */
    int mNrrDepDiag = 0; /* [debug] nrr_hud_depdiag (n60intro): log which entry words a program missed on */
    int mNrrWide = 7; /* [native] nrr_hud_wide: 1 virtual SETTIMG width > 1 loads, 2 page-keyed atlas states,
                       * 4 virtual segmented-operand loads */
    int mNrrHudMemo = 0;
    int mNrrKept = 0; /* [native] nrr_hud_kept (n60hudx) */
    int mNrrRunClock = 0; /* [debug] nrr_hud_runclock (n60hudx) */
    int mNrrRr = 0;       /* [native] nrr_hud_rr (n60hudx) */
    int mNiMenu = 0;      /* nimenu [native] ni_menu */
    struct NiMenuWin {
        uint32_t runs, words, prog, dec, stockRuns, stockWords, tileIds, vol;
    } mNiMenuWin = {};
    static constexpr uint32_t kNiVolSlots = 256; /* run ordinals per task tracked (power of two) */
    static constexpr uint32_t kNiVolTrip = 4;    /* decoded this many times in a row: volatile */
    static constexpr uint32_t kNiVolRetry = 16;  /* a volatile run is offered to the decoder again after this */
    uint8_t mNiMenuVol[kNiVolSlots] = {};
    uint32_t mNiMenuOrd = 0;
    bool mNiMenuRun = false; /* inside a MENU run (the decoder's MENU-only arms: NrrNoteLoadTile) */
    int mNiVolDiag = 0;      /* menus-s2 [debug] nimenu_voldiag: lines left */
    struct NiVolCopy;
    NiVolCopy* mNiVolCopy = nullptr;
    void NiMenuVolDiag(const GdxHudWord* w, uint32_t n, uint32_t ord);
    uint32_t mNrrRunInTask = 0;
    bool mNrrTmemLive = false;
    bool mNrrOpen = false;     /* the NRR core holds citro3d (between HUD runs too): anything else closes it */
    /* A Flush a register setter makes with no batch open (prim depth, fog / blend colour, the fog
     * line, prim / env) drains nothing and touches no backend state: it leaves the hand-off open.
     * Every other reason is a state block, a draw, a hook or the task end. */
    static bool NrrFlushTouchesBackend(FlushReason why) {
        return why != FLUSH_PRIMDEPTH && why != FLUSH_FOGCOLOR && why != FLUSH_BLENDA && why != FLUSH_FOG &&
               why != FLUSH_PRIM && why != FLUSH_ENV;
    }
    uint32_t mNrrTexEpoch = 0; /* every texture-cache retire (a clear's are counted here too) */
    uint32_t mNrrSlotGen[kTexCacheSize] = {}; /* per cache slot: bumped when the slot is retired */
    void NrrCensusRun(const void* runWords, uint32_t n, uint32_t runIndex, uint32_t frame);
    bool NrrEnsure();
    /* hot (tools/native/nrr_size.sh counts NrrHudHot*) */
    bool NrrHudHotRun(const void* runWords, uint32_t n, uint32_t mode);
    /* nimenu: the menu phase's runner (gdx_nrr_menu.inc; nrr_size.sh counts NrrMenuHot*) */
    bool NrrMenuHotRun(const void* runWords, uint32_t n);
    bool NrrMenuHotBody(const GdxHudWord* w, uint32_t n);
    bool NrrMenuDec(const GdxHudWord* w, uint32_t n);
    int NrrMenuSplit(const GdxHudWord* w, uint32_t n);
    bool NrrMenuSegment(const GdxHudWord* w, uint32_t n);
    bool NrrMenuRunCold(const GdxHudWord* w, uint32_t n);
    void NrrNoteLoadTile(const Cmd& c);
    void NrrHudHotBody(const GdxHudWord* w, uint32_t n);
    const NrrProg* NrrHudHotProg(const GdxHudWord* w, uint32_t n);
    void NrrHudHotExec(const NrrProg& p, const GdxHudWord* w, uint32_t n, const uint32_t* op, uint32_t pos);
    /* n60hudx [native] nrr_hud_rr */
    const uint32_t* NrrHudHotRrTry(const NrrProg& p, const GdxHudWord* w, uint32_t n, uint32_t* pos);
    bool NrrHudHotRectFrom(const uint32_t* q, const GdxHudWord* w, const float* f);
    void NrrHudHotGroups(const uint32_t* g, const uint32_t* end);
    void NrrRrArm(const NrrProg& p);
    void NrrRrGateOut(const NrrProg& p);
    void NrrRrCapBegin(const NrrProg& p, bool verify);
    void NrrRrCapture(const uint32_t* op, const NrrBlock* b, const float* p);
    void NrrRrCapEnd(const NrrProg& p, const GdxHudWord* w, uint32_t n);
    bool NrrRrBuild(const NrrProg& p, const GdxHudWord* w, uint32_t n);
    void NrrRrVerify(const NrrProg& p, uint32_t n);
    void NrrHudHotRrSnap(uint32_t* out) const;
    void NrrRrReset();
    const uint32_t* NrrHudHotStores(uint32_t hdr, const uint32_t* st);
    void NrrHudHotLoad(const uint32_t* r, const Cmd& c);
    void NrrHudHotSlot(uint32_t lo, uint32_t hi, const uint32_t* r);
    bool NrrHudHotIdent(const uint32_t* src, uint32_t lo) const;
    void NrrHudHotDraw();
    bool NrrHudHotSplits(const NrrBlock& b, uint32_t cls);
    void NrrHudHotBatch(NrrBlock& b, bool noSc, uint32_t cls);
    float* NrrHudHotReserve(NrrBlock& b);
    void NrrHudHotClose();
    void NrrHudHotCorners(const NrrBlock& b, const int32_t e[4], const float st[4], bool flip, float* out) const;
    void NrrHudHotCornersAt(const NrrBlock& b, const int32_t e[4], const float st[4], bool flip, const Rgba* colSrc,
                            const Rgba* alphaSrc, float* out) const;
    bool NrrHudHotGeom(const NrrBlock& b, const Cmd& c, const GdxHudWord* t, int32_t e[4], float st[4],
                       bool* flipOut) const;
    void NrrHudHotRect(NrrBlock* b, const Cmd& c, const GdxHudWord* t, const uint32_t* op);
    NrrBlock* NrrScVariant(NrrBlock* base, const Cmd& c, const GdxHudWord* t, bool fill);
    void* NrrScStateMem(uint32_t owner);
    void NrrLateFlush();
    NrrBlock* NrrScVariantOf(NrrBlock* base, const NrrKey& k, uint32_t h);
    NrrBlock* NrrScSibling(const NrrKey& k);
    NrrBlock* NrrScPromote(uint32_t var, uint32_t h);
    NrrBlock* NrrScBaseOf(NrrBlock* b);
    bool NrrLdPitchOk(const uint32_t* r, const Cmd& c);
    struct NrrRectMemo;
    NrrRectMemo* NrrHudHotMemoFind(const NrrBlock& b, const Cmd& c, const GdxHudWord* t, const uint32_t* op);
    void NrrHudHotMemoKey(const NrrBlock& b, const Cmd& c, const GdxHudWord* t, const uint32_t* op,
                          uint32_t key[kNrrMemoKeyWords]) const;
    /* cold: the per-word decoder (compiles programs, finishes what a program cannot) */
    void NrrDecRun(const GdxHudWord* w, uint32_t n);
    void NrrDecFrom(const GdxHudWord* w, uint32_t n, uint32_t i);
    uint32_t NrrProgEx(const NrrProg& p, const GdxHudWord* w, uint32_t n, uint32_t i);
    void NrrProgMiss(const NrrProg& p, const GdxHudWord* w, uint32_t n, uint32_t i);
    void NrrRec(uint32_t hdr, const Cmd& c, const uint32_t* payload, uint32_t words);
    void NrrRecEx(uint32_t i);
    void NrrRecOpaque(uint32_t i);
    void NrrRecBegin(bool ok);
    uint32_t NrrSnapIndex(const void* p) const;
    void NrrRecRead(const void* p, uint32_t len);
    void NrrRecWrote(const void* p, uint32_t mask);
    void NrrRecSt(uint32_t i, const Cmd& c, uint32_t hdr, const uint32_t* st);
    void NrrRecLd(uint32_t i, const uint32_t* r, const Cmd& c);
    void NrrRecWideReads(uint8_t loadTile);
    void NrrVerifyLoadedAs(const Cmd& c, const uint32_t* r);
    uint32_t NrrStateTex(const NrrBlock& b) const;
    const NrrState* NrrHudState(const NrrStateKey& k);
    const NrrState* NrrOwnState(const NrrStateKey& k);
    bool NrrCoreTableFlush();
    bool NrrVerifyBindsSame(const NrrBlock& b) const;
    void NrrRecTl(uint32_t i, const Cmd& c);
    void NrrRecRect(uint32_t i, const Cmd& c, const NrrBlock* b, uint8_t tile, bool fill);
    void NrrRecRegWord(const Cmd& c, uint32_t i, const NrrRegFx* fx, bool plain);
    bool NrrColourOp(const Cmd& c, uint32_t col[3]) const;
    bool NrrPrimDepthOp(uint32_t col[3]) const;
    static uint32_t NrrColourHdr(uint8_t op);
    bool NrrRegFirstSight(const Cmd& c) const;
    bool NrrNoteNoProg(const GdxHudWord* w, uint32_t n);
    void NrrNoHashDiag(const GdxHudWord* w, uint32_t n);
    void NrrDepDiag(const NrrProg& g, const GdxHudWord* w, uint32_t n);
    void NrrDepDiagTask();
    void NrrHudHotColour(uint32_t hdr, const uint32_t* op, const Cmd& c);
    /* n60hudx [native] nrr_hud_kept: an SL op (a recorded G_DL to a static setup list) */
    bool NrrHudHotSl(const uint32_t* op, const Cmd& c, const NrrProg& p, const GdxHudWord* w, uint32_t n, uint32_t i);
    void NrrSlMiss(const NrrProg& p, const GdxHudWord* w, uint32_t n, uint32_t i, const ListRef& sub, int why);
    uint32_t NrrDecDl(const GdxHudWord* w, uint32_t n, uint32_t i, bool rec, const ListRef* pre);
    int NrrSlCompile(const ListRef& sub, uint32_t i, uint32_t* pay, uint32_t* words);
    uint32_t NrrVerifyDl(const GdxHudWord* w, uint32_t n, uint32_t i, const uint32_t* opPay, bool rec);
    bool ResolveDlAsExec(const Cmd& c, ListRef& sub, bool* hookTaken);
    void WalkNestedList(const ListRef& sub);
    void HudNestedDl(const Cmd& c);
    void HudReplayRunPlain(const GdxHudWord* w, uint32_t n);
    void HudReplayMenuRun(void* record, const GdxHudRun& run); /* nimenu */
    void HudReplayMenuStock(const GdxHudWord* w, uint32_t n);
    uint32_t NrrDecWord(const GdxHudWord* w, uint32_t n, uint32_t i, bool rec);
    uint32_t NrrDecReg(const Cmd& c, uint32_t i, bool rec);
    void NrrDecLoad(const Cmd& c, uint32_t i, bool rec);
    bool NrrLoadRecord(const Cmd& c, uint32_t r[5]);
    bool NrrSegResolve(const Cmd& c, uint32_t line, uint32_t bytes, uint32_t r[5]);
    void NrrRecLoad(uint32_t i, const uint32_t* r, const Cmd& c);
    void NrrDecRect(const Cmd& c, const GdxHudWord* t, uint32_t i, bool rec);
    bool NrrDecKey(NrrKey& k, uint8_t tile, bool fill) const;
    NrrBlock* NrrDecFind(const NrrKey& k, uint32_t* hashOut);
    NrrBlock* NrrDecBlockFor(uint8_t tile, bool fill, const Cmd& c, const GdxHudWord* t);
    bool NrrRegApply(const NrrRegFx& fx);
    static uint32_t NrrKeyHash(const NrrKey& k);
    void NrrProgClear();
    void NrrProgStore(const GdxHudWord* w, uint32_t n);
    void NrrSnapInit();
    void NrrSnapAdd(const void* p, uint32_t len);
    void NrrSnapTake(uint32_t* dst) const;
    /* cold */
    bool NrrHudOpen();
    void NrrHudClose();
    void NrrSimDefaultViewport();
    void NrrStockViewportScissor();
    bool NrrResolveNoScissor(NrrBlock& b);
    void NrrRunCold(const GdxHudWord* w, uint32_t n);
    void NrrRunEpochs();
    uint32_t NrrExecWord(const Cmd& c, uint32_t i);
    uint32_t NrrColdWord(const Cmd& c, uint32_t i);
    void NrrRegEnv(uint32_t env[8]) const;
    void NrrRegClear();
    int NrrRegRegions(uint8_t* base[3], uint32_t len[3]);
    uint32_t NrrRegKeepMask(const uint8_t* word) const;
    uint32_t NrrRegCold(const Cmd& c, uint32_t i, uint32_t h, const NrrRegFx** fxOut);
    void NrrRegCompile(NrrRegFx& fx, const Cmd& c, uint32_t i);
    uint32_t NrrFxHeader(const NrrRegFx& fx, uint32_t* st) const;
    NrrBlock* NrrBlockMiss(const NrrKey* key, uint32_t h, NrrBlock* b, const Cmd& c, const GdxHudWord* t, bool fill);
    bool NrrBlockFresh(NrrBlock& b);
    void NrrAuxCold(const NrrBlock& b, float out[7][4]);
    void NrrAuxSelect(NrrBlock& b, bool* exotic);
    void NrrHudHotAux(const NrrBlock& b, uint32_t prim, uint32_t env, uint32_t shade, float out[7][4]) const;
    uint32_t NrrInternClass(const NrrStateKey& k);
    void NrrNoRing(const Cmd& c, const GdxHudWord* t, bool fill);
    void NrrLoadCold(const Cmd& c);
    void NrrNoteTlut(const Cmd& c);
    bool NrrBlockRecheck(NrrBlock& b);
    void NrrRectXFull(int32_t ulx, int32_t lrx, float* x0, float* x1) const;
    void NrrXMode();
    void NrrFillFullScreen(int32_t e[4]) const;
    void NrrCornersCold(const NrrBlock& b, const int32_t e[4], const float st[4], bool flip, float* out) const;
    void NrrStockRect(const Cmd& c, const GdxHudWord* t, bool fill, const NrrKey* k, uint32_t hash);
    void NrrCaptureAfter(const NrrKey& key, uint32_t hash, uint8_t tile, bool fill, uint32_t emit0, const Cmd& c,
                         const GdxHudWord* t);
    bool NrrBlockCapture(NrrBlock& b, uint8_t tile, bool fill);
    bool NrrResolveState(NrrBlock& b);
    void NrrSelectInputs(NrrBlock& b);
    bool NrrBlockSelfCheck(const NrrBlock& b, const Cmd& c, const GdxHudWord* t, bool fill);
    void NrrMaterializeAll();
    void NrrMaterializeAt(uint32_t i);
    void NrrPendMakeRoom();
    void NrrDropPendingAt(uint32_t i);
    void NrrTmemWrite(uint32_t off, uint32_t n);
    void NrrTmemRead(int tile);
    void NrrUpdateLive();
    void NrrMemoClear();
    void NrrDebugBatch(const NrrBlock& b, const Gdx3dsVboPackAux& aux);
    void NrrDebugStock(const void* runWords, uint32_t at);
    void NrrDebugRect(const NrrBlock& b, const float* v);
    /* [native] nrr_hud=2 */
    void NrrVerifyRun(const GdxHudWord* w, uint32_t n);
    uint32_t NrrVerifyWord(const GdxHudWord* w, uint32_t n, uint32_t i, bool rec);
    void NrrVerifyProg(const NrrProg& p, const GdxHudWord* w, uint32_t n);
    void NrrVerifyBad(const char* what, const Cmd& c, uint32_t a, uint32_t b);
    uint32_t NrrVerifyLoad(const Cmd& c, uint32_t i, const uint32_t* opRec, bool rec);
    uint32_t NrrVerifyReg(const Cmd& c, uint32_t i, const uint32_t* opSt, uint32_t opHdr, bool rec);
    void NrrVerifyRecReg(const Cmd& c, uint32_t i, uint32_t h);
    uint32_t NrrVerifyRect(const Cmd& c, const GdxHudWord* t, uint32_t i, NrrBlock* opBlock, bool rec);
    void NrrVerifyRectOut(const NrrBlock& b, const Cmd& c, const float* mine, uint32_t emit0);
    void NrrVerifyAux(const NrrBlock& b);

    struct HudMemo; /* gdx_hud_replay.cpp */
    HudMemo* mHudMemo = nullptr; /* allocated on first replay */
    HudMemo* mHud = nullptr;     /* == mHudMemo while a run replays, else nullptr */
    int mHudLevel = 0;           /* the replaying record's mode: 1 use the memos, 2 verify them */
    bool mHudCensus = false;
    /* The replayed run has executed only rectangles and colour-register writes since the last
     * rectangle: every input of ResolveDrawState is what it was then (gdx_hud_replay.cpp). */
    bool mHudRectFast = false;
    bool HudRectState(bool isRect, DrawState& out);
    bool ResolveTextureSourceStock(size_t requiredBytes);
    bool HudResolveTextureSource(size_t requiredBytes);
    bool HudHashLookup(const uint8_t* addr, uint32_t n, uint32_t* out);
    void HudHashStore(const uint8_t* addr, uint32_t n, uint32_t h);
    bool HudTmemHashLookup(uint16_t base, uint32_t span, uint32_t* out);
    void HudTmemHashStore(uint16_t base, uint32_t span, uint32_t h);

    void SpReset();
    void Walk(const ListRef& root);
    void WalkCensus(); /* [native] dspcensus: Walk with the per-opcode/per-phase clocks */
    void WalkNicensus(); /* [debug] nicensus: Walk with NcNote per command (gdx_nicensus.cpp) */
    NcState* mNc = nullptr; /* [debug] nicensus: null = off */
  public:
    /* nirace [native] ni_race (gdx_nirace.cpp): the words between the root list's markers of a race
     * frame, compiled into span programs. 0 off, 1 on, 2 verify (every compiled register group checked
     * against its stock handlers). Set once (allocates); armed per task by the glue (race scope). */
    void NrrRaceSet(int level, int log);
    /* nirace [native] ni_race_mesh: a geometry segment of a span program (its G_VTX words + one run of
     * triangles, a narrow 8-byte-packet list ending in G_ENDDL) drawn by the mesh path (the glue: the
     * dress registry); hint = the list the words came from. Returns 1 when drawn. */
    typedef int (*NiSegHook)(void* user, const void* seg, size_t cmds, const void* hint);
    void NrrRaceSetSegHook(NiSegHook fn, void* user) {
        mNiSeg = fn;
        mNiSegUser = user;
    }
    void NrrRaceArm(bool on) {
        mNiOn = on && mNi != nullptr;
    }
    /* menus-s2 [native] ni_menu_root: arm (or disarm) the programs for a menu frame's root -- a race
     * arm with the menu flag (no geometry segments; an open NRR HUD hand-off at a span's start is
     * closed so the span's program can run). false disarms either kind. */
    void NrrRaceArmMenu(bool on);
    /* native-vs: the task is a 2-4P race frame (armed with [native] ni_view's NRR_NI_VIEW_RACE bit):
     * its spans are keyed on their occurrence after the same marker word instead of the frame's span
     * ordinal (NrrRaceViewKey). Set by the glue before NrrRaceArm, cold. */
    void NrrRaceSetMultiView(bool on);
    bool NrrRaceArmed() const {
        return mNiOn;
    }
    /* The window's receipt fields (programs, ops, bails, verify) appended to buf; resets the window. */
    int NrrRaceReceipt(char* buf, int cap);
    /* [debug] nirace_rcpt (nimenu W4, review §4 hot trims): whether the periodic receipt window is
     * even going to print ([native] gdx_rt_glue.cpp's sNiRcpt > 0, the window-length lever, default
     * 256 whenever ni_race is armed at all). When false, the kNiEnd span-end tally
     * (NrrRaceEndReceipt) is skipped outright instead of accumulating into counters nothing will
     * ever read. Mirrored here (not read from gdx_rt_glue.cpp directly) so the hot dispatch only
     * pays one bool test, not a cross-TU call. */
    void NrrRaceRcptSet(bool on) {
        mNiRcpt = on;
    }

  private:
    struct NrrRace;
    struct NrrRaceSlot;
    NrrRace* mNi = nullptr;
    bool mNiOn = false;
    bool mNiRcpt = false;
    NiSegHook mNiSeg = nullptr;
    void* mNiSegUser = nullptr;
    void NrrRacePlanGeo(const ListRef& L, size_t a, size_t b, size_t base);
    bool NrrRaceSegFallback(const uint32_t* seg, uint32_t cmds);
    void NrrRaceNoteEx(const Cmd& c); /* [debug] nicensus (nimenu W2): cold trampoline for kNiEx's NcNote */
    void NrrRaceEndReceipt(size_t start, size_t r, uint32_t c0, uint32_t c1); /* nimenu W4: kNiEnd's cold tally */
    void NrrRaceSegRun(const uint32_t* seg, uint32_t cmds, const void* hint, NrrRaceSlot* slot);
    void NrrRaceTaskBegin(); /* the root's walk starts: the environment the compiled scissors rest on */
    void NrrRaceHotDrive(Frame& f, uint32_t marker);
    bool NrrRaceHotSpan(Frame& f, uint32_t marker);
    uint32_t NrrRaceViewKey(uint32_t marker); /* native-vs: a multi-view span's key (cold) */
    bool NrrRaceMissRecover();
    bool NrrRaceMenuReopen(); /* menus-s2 ni_menu_root: close the NRR HUD hand-off, run the span (cold) */
    bool NrrRaceHotRun(const uint32_t* op, Frame& f, NrrRaceSlot* slot);
    const uint32_t* NrrRaceGeoOp(const uint32_t* op, Frame& f, const ListRef& L, NrrRaceSlot* slot, size_t start);
    void NrrRaceHotCall(const Cmd& c, Frame& t);
    bool NrrRaceMiss(Frame& f, uint32_t marker, const Cmd& c, uint32_t h);
    bool NrrRaceCompile(Frame& f, NrrRaceSlot* slot);
    int NrrRaceCompileWord(const Cmd& c, const ListRef& list, uint32_t idx, uint32_t* st, uint32_t* ns);
    bool NrrRaceCompileList(const ListRef& sub);
    void NrrRaceBail(NrrRaceSlot* slot, size_t start, size_t resume, int why, size_t var, uint32_t expW0);
    void NrrRaceStOther(const uint32_t* op, const ListRef& list, size_t idx, bool root);
    void NrrRaceVerifySt(const uint32_t* op, const ListRef& list, size_t idx, bool root);
    /* nicourse [native] ni_course (gdx_nicourse.cpp) */
    bool NrrCourseHotNextIsModel(const Frame& f) const;
    bool NrrCourseHotOffscreen() const;
    void NrrCourseHotKey(void* key) const;
    int NrrCourseHotDraw(const Gdx3dsCourseModel& m, const void* memo, void* seam);
    int NrrCourseCold(const Gdx3dsCourseModel& m, const void* key, void* seam);
    void NrrCourseLeave(Frame& f, size_t first, const uint32_t tail[2]);
    void NrrCourseVerify(uint32_t nCmds, uint32_t w1, const Cmd& second, const Frame& f, void* seam);
    bool RemapF3d(Cmd& c, const ListRef& list, size_t* f3dXferSize);
    int Exec(const Cmd& c, Frame& f, size_t f3dXferSize);
    bool PushList(const ListRef& list);

    /* --- gdx_rsp.cpp --- */
    void SpMatrix(uint8_t params, const Cmd& c, const ListRef& list);
    void SpMatrixReplayHook(uint8_t params, uintptr_t p, const int32_t* words); /* nrrdiet: cold, [native] replay only */
    void SpPopMatrix(uint32_t count);
    void SpVertex(size_t n, size_t dest, const Cmd& c, const ListRef& list);
    void SpModifyVertex(uint16_t idx, uint8_t where, uint32_t val);
    void SpMovemem(uint8_t index, uint8_t offset, const Cmd& c, size_t xferSize, const ListRef& list);
    void SpDmaIo(const Cmd& c);
    void SpLine3d(uint8_t v1, uint8_t v2, uint8_t halfWidth);
    void CalculateNormalDir(const Light* light, float coeffs[3]);
    float AdjXForAspectRatio(float x) const;

    /* --- gdx_interp.cpp (pure state) --- */
    void SpGeometryMode(uint32_t clear, uint32_t set);
    void SpMoveword(uint8_t index, uint16_t offset, const Cmd& c);
    void SpTexture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on);
    void SpSetOtherMode(uint32_t shift, uint32_t numBits, uint64_t mode);
    void DpSetOtherMode(uint32_t h, uint32_t l);
    void DpSetTextureImage(uint32_t fmt, uint32_t siz, uint32_t width, const Cmd& c);
    void DpSetCombineMode(uint32_t rgb, uint32_t alpha, uint32_t rgb2, uint32_t alpha2);
    void DpSetEnvColor(Rgba c);
    void DpSetPrimColor(uint8_t m, uint8_t l, Rgba c);
    void DpSetFogColor(Rgba c);
    void DpSetBlendColor(Rgba c);
    void DpSetFillColor(uint32_t packed);
    void DpSetPrimDepth(uint16_t z);
    void DpSetColorImage(uint32_t fmt, uint32_t siz, uint32_t width, const Cmd& c);
    void DpSetZImage(const Cmd& c);

    /* --- gdx_tmem.cpp --- */
    void DpSetTile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                   uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts);
    void DpSetTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt);
    void DpLoadTlut(uint8_t tile, uint32_t highIndex);
    void DpLoadBlock(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);
    uint32_t LoadBlockLineBytes(uint8_t tile, uint16_t tmemIndex, uint32_t size_bytes, uint32_t dxt) const;
    /* [native] nrr_hud: DpLoadBlock's line pitch while a virtual load is materialised (0 = its own
     * decision, LoadBlockLineBytes) -- the pitch the load had when it was recorded */
    uint32_t mLoadLineForce = 0;
    /* [native] nrr_hud: the source a materialised virtual load of a segmented operand resolved to
     * when it was recorded (ResolveTextureSource serves it; flags bit 0 immutable, bit 1 native) */
    const uint8_t* mLoadSrcForce = nullptr;
    uint8_t mLoadSrcFlags = 0;
    void DpLoadTile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
    bool ResolveTextureSource(size_t requiredBytes);
    void StoreLoadedTexture(uint16_t start, const LoadedTexture& t);
    void NoteTmemLoadKind(uint16_t base, uint8_t kind, uint32_t writeEndByte);
    /* Everything SameContentLoad tests EXCEPT the source bytes: the live span's geometry and the
       full LoadedTexture descriptor. Split out so the immutable gate and the [native] tmemskip
       staleness gate are two different answers to "are the SOURCE BYTES the same", asked over
       one shared, unchanged definition of "is everything else the same". */
    bool SameContentDescriptor(uint16_t base, uint8_t kind, const LoadedTexture& fresh) const;

    /* [memotear] Fault injection for the tmemskip refusal arm: called on a load that HAS been
       admitted and whose stamp/class both still match, i.e. the load that is about to skip.
       Returns true when it has forced the world stale (and says which way through `outKind`),
       having applied the fault to the state the gate is about to read. */
    bool TmemInjectStaleness(uint16_t base, unsigned cls, uintptr_t src, uint32_t readBytes);
    bool SameContentLoad(uint16_t base, uint8_t kind, const LoadedTexture& fresh) const;
    /* [native] tmemskip: decide the load, stamp it, and keep the receipt. Returns true when the
       mirror + span store can be skipped outright; otherwise fills `outStamp`/`outCls` with the
       stamp to record once the mirror has been written. */
    bool TmemLoadIsRedundant(uint16_t base, uint8_t kind, const LoadedTexture& fresh, uint32_t readBytes,
                             uint64_t outStamp[2], unsigned* outCls);
    void RecordTmemSrcStamp(uint16_t base, const uint64_t stamp[2], unsigned cls);
    void NoteDescriptorMiss(uint16_t base, uint8_t kind, const LoadedTexture& fresh);
    void MirrorToTmem(uint8_t* dst, const uint8_t* src, uint32_t n, bool nativeRgba16);
    void ArmTexturesChanged();

    /* --- gdx_texcache.cpp --- */
    void ImportTexture(int unit, int tile);
    bool TexCacheLookup(int unit, const TexKey& key);
    void DecodeRgba16(int unit, int tile, const LoadedTexture& lt);
    void DecodeRgba32(int tile, const LoadedTexture& lt);
    void DecodeIA4(int tile, const LoadedTexture& lt);
    void DecodeIA8(int tile, const LoadedTexture& lt);
    void DecodeIA16(int tile, const LoadedTexture& lt);
    void DecodeI4(int tile, const LoadedTexture& lt);
    void DecodeI8(int tile, const LoadedTexture& lt);
    void DecodeCi4(int tile, const LoadedTexture& lt);
    void DecodeCi8(int tile, const LoadedTexture& lt);
    bool UploadPrepare(uint32_t w, uint32_t h);
    void UploadNeutralFallback(int unit);
    void Upload(uint32_t w, uint32_t h);
    void TexCacheRetireSlot(int16_t slot);
    void TexCacheSignalRetire(int16_t slot);
    bool TexKeepEligible(const TexEntry& e) const;
    void TexCacheClearKeeping();
    void TexKeepRelieve();
    void TexKeepLog() const;

    /* --- gdx_tri.cpp --- */
    /* [native] trimemo: is every input ResolveDrawState reads identical to the last full
       resolve's? See mTmKey. Bank records the key after a full resolve. */
    bool TriMemoHit(bool isRect) const;
    void TriMemoBank(bool isRect, const DrawState& ds);
    void* LookupOrCreateShader(uint64_t id0, uint64_t id1);
    /* True when a VALUE change of prim (env) colour can ride per vertex in the OPEN packed
     * batch, i.e. no TexEnv stage constant of its shader reads that register. Mirrors the LUS
     * GdxHudRegFoldable ([native] fx_hud) over the same backend contract. */
    bool RegFoldable(bool isPrim) const;
    void NoteRegChange(bool isPrim, FlushReason why);

    /* --- gdx_rect.cpp --- */
    void DpSetScissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry);
    void CalcAndSetViewport(const int16_t* vp /* vscale[4], vtrans[4] */);
    void AdjustViewportOrScissor(Rect* area);
#ifdef EXPANSION_KIT
    /* B11o: widen a scissor's x span to the union of the linear (viewport) mapping and the
     * 2D pillarbox mapping DrawRectangle uses. gdx_rect.cpp carries the derivation. */
    void ScissorPillarSafeX(float n64X, float n64W);
#endif
    void DrawRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);
    void DpTextureRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                            int16_t ult, int16_t dsdx, int16_t dtdy, bool flip);
    void DpFillRectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry);

    Backend* mBackend;
    DlSrc mSrc;
    Rsp mRsp;
    Rdp mRdp;
    RenderState mRs;
    Counters mStats;
    GdxFrameConfig mCfg;
    int mVariant = 0;
    bool mLineSection = false;
    /* [nverify] latched once per task in RunFrame (the glue arms/disarms verify immediately
     * around that call). The per-command and per-triangle diagnostics in gdx_dl.cpp /
     * gdx_tri.cpp are gated on this bool, so they cost one predictable branch when
     * [native] interp_verify=0. Audit 2026-09-23 finding 31. */
    bool mVerifyActive = false;

    ListRef mBranchZTarget;
    bool mBranchZValid = false;

    Frame mStack[kCallStack];
    int mSp = 0;

    /* Combiner pool + texture cache. */
    CombinerPool mCcPool;
    TexEntry mTex[kTexCacheSize];
    int16_t mTexHash[kTexCacheHash];
    int16_t mTexLruHead = -1, mTexLruTail = -1;
    unsigned mTexCount = 0;
    uint32_t mTexFreeIds[kTexCacheSize];
    unsigned mTexFreeIdCount = 0;
    bool mHashCiRgba32 = true;
    uint8_t* mTexUpload = nullptr;
    uint32_t mTexUploadCap = 0;   /* bytes mTexUpload can currently hold */
    uint32_t mTexUploadMax = 0;   /* [debug] texscratchmaxkib; 0 = kTexUploadMaxBytes */
    bool mUploadDone = false;     /* did this import actually reach Upload()? */
    uint8_t mFallbackTexels[kFallbackTexBytes]; /* opaque white, DROPSITE 1's neutral upload */
    uint8_t mDropsiteTexLogged = 0;  /* one-shot DROPSITE line for the decode ceiling */
    uint8_t mDropsiteVtxLogged = 0;  /* one-shot DROPSITE line for an over-range G_VTX */
    uint8_t mDropsiteClipLogged = 0; /* one-shot DROPSITE line for a truncated clip polygon */
    uint8_t mDropsiteStaleLogged = 0;/* one-shot DROPSITE line for an import that uploaded nothing */
    /* Session high-water marks -- deliberately NOT in Counters, which ResetStats() clears every
     * receipt window. A drop site's margin is only meaningful over the whole run. */
    uint32_t mTexDecodeHwBytes = 0; /* largest decode the run ever asked the scratch for */
    uint32_t mClipPolyHw = 0;       /* largest clip-polygon vertex count the run ever produced */
    uint32_t mVtxHwEnd = 0;         /* largest dest_index + n_vertices a G_VTX ever asked for */

    /* Batch emission state ([triloop] packed VBO). */
    size_t mBufNumTris = 0;
    float* mPackDst = nullptr;
    bool mPackBatch = false;
    /* GEOMLOSS: triangles this batch may hold before it must be flushed. kMaxTriBuffer in
     * the normal case; lower when the backend's pool cannot hold a whole batch, which is
     * where the old code returned early and lost the geometry outright. */
    size_t mPackCapTris = static_cast<size_t>(kMaxTriBuffer);
    Gdx3dsVboPackParams mPackParams;
    Gdx3dsVboPackAux mPackAux;
    uint32_t mClipFanDepth = 0;
    bool mTrectRectAtlasOk = false;
    bool mTrectBatchOn = false;
    float mByteToUnit[256];

    /* The tri memo: valid from a resolved state until the batch it belongs to is flushed. */
    DrawState mTriMemo;
    bool mTriMemoValid = false;
    /* The combiner the OPEN packed batch was begun under: the fold key (== LUS sGdxPackComb). */
    const Combiner* mPackComb = nullptr;
    /* A batch that absorbed a prim/env fold must not take the [trectbatch] deferred import
     * (== LUS sGdxPackFolded); see fx-progress.md "Frame 1418". */
    bool mPackFolded = false;
    int mCensus = 0;  /* [native] ncensus */
    int mOwnFold = 0; /* [native] ownfold */
    int mDspCensus = 0; /* [native] dspcensus: 0 off, 1 per-opcode self time, 2 + walk phases */
    uint32_t mVtxSoftCap = static_cast<uint32_t>(kMaxVertices); /* [debug] vtxmax */
    bool mVtxClamp = true;                                      /* [native] vtxclamp */
    bool mTexStaleFix = true;                                   /* [native] texstalefix */
    uint32_t mTexKeepBytes = 0;      /* N60TEX [debug] tex_keep budget; 0 = the stock clear */
    uint32_t mTexKeepFloorBytes = 0; /* N60TEX: no keeping below this much linear heap */
    uint16_t mTexGen = 0;            /* N60TEX: bumped by every clear */
    bool mTexKeptNone = true;        /* N60TEX: no entry of an older generation is left */
    TexKeepStats mTexKeep = {};
    uint32_t mClipPolyMax = 16;                                 /* [debug] clippolymax */
    unsigned mTexCountHw = 0;                                   /* B23: LRU occupancy high-water */
    SessionDrops mSesDrops = {};                                /* never reset by ResetStats */
    int mTimgMemo = 0;  /* [native] timgmemo: cross-frame SETTIMG classification memo (B4/F3) */
    uint32_t mTimgEpochLast = 0; /* [native] timgmemo receipt: epoch-churn detector */
    /* [native] tmemskip -- widen the same-content skip past the o2r resource class, using the
     * generation/DMA staleness test the bridge already runs (gdx_dlsrc_bridge.h). 0 off,
     * 1 the two classes that are stable WITHOUT a compare (native-RGBA16 by generation, and
     * registered ROM-backed host ranges), 2 additionally the DMA-tracked class (RDRAM and
     * framebuffer shadows), whose generation does NOT see a plain CPU store into RDRAM.
     * Default 0. The invalidation argument is docs/research/native/tmemblock-progress.md S2. */
    int mTmemSkip = 0;
    int mTmemCls = 0; /* [native] tmemcls: class census only; changes no behaviour */
    /* [memotear] [debug] tmemfault -- fault injection for the refusal arm. 0 off (default and
     * the only value the shipped build ever resolves); N forces staleness on every Nth load
     * that would otherwise have skipped. Inert unless mTmemSkip != 0. See
     * docs/research/native/memotear-progress.md S7. */
    int mTmemFault = 0;
    uint32_t mTmemFaultTick = 0;
    uint32_t mBatchesAtTaskStart = 0;

    /* [dspx]/[trix] clocks: latched per task in RunFrame from the backend's [prof] gate (and
       off under [native] dspcensus, which runs its own heavier clocks). */
    bool mDxOn = false;
    uint32_t mTriSampleSeq = 0;
    /* [native] o3dsfast (default 1): the EXACT rewrites -- same stores, same floats, fewer
       instructions (halfword swap by word, span-store range fills, a clipper without `%`,
       which ARMv6 has no divider for). 0 restores the historic loops for an A/B. */
    int mO3dsFast = 1;
    /* [native] trimemo (default 0): skip ResolveDrawState when nothing it reads has changed. */
    int mTriMemoOn = 0;
#ifdef EXPANSION_KIT
    /* [native] ek_editmesh (gdx_ekpreview.cpp): 0 off, 1 serve, 2 verify (stock draws, the memo is
     * compared). mEkCap is non-null only while an element's stock draw is being recorded. */
    int mEkPvOn = 0;
    EkPvMemo* mEkPv = nullptr;
    EkPvCap* mEkCap = nullptr;
    bool EkPvVtx(const Cmd& c, Frame& f);
    void EkPvStep(const Cmd& c, Frame& f);
    void EkPvStock(const Cmd& vtx, Frame& f, int kind, EkPvCap* cap);
    int EkPvServe(const EkPvEntry& e, int ctxSlot);
    void EkPvCaptureTri(const float* out, const LoadedVertex* v0, const DrawState& ds);
    void EkLatchAux(const DrawState& ds, const LoadedVertex& v0); /* gdx_tri.cpp: SpTri's latch */
#endif
    /* Bumped by every span-store mutation (StoreLoadedTexture): LoadedTexAt is a function of
       the store, so the tri memo keys on this instead of trusting textures_changed alone. */
    uint32_t mSpanEpoch = 0;
    /* The tri memo's key: a SNAPSHOT of every input ResolveDrawState reads, taken right after
       a full resolve. A hit requires every field equal (memcmp where a struct is compared, so
       padding can only cause a spurious MISS, never a hit). The native replay hooks write the
       RDP/RSP registers through GdxSeamRegs pointers without passing through a handler, which
       is why this is a snapshot compare and not a dirty flag: a hook's direct write shows up
       as an unequal field. */
    struct TriMemoKey {
        bool valid;
        bool isRect;
        uint8_t firstTile;
        bool grayscale;
        uint32_t otherModeL, otherModeH, geometryMode;
        uint64_t combineMode;
        uint32_t spanEpoch;
        Tile tiles[2];     /* the texel0/texel1 tiles, whole */
        RenderState rs;    /* what the backend was left holding */
    };
    TriMemoKey mTmKey = {};
    DrawState mTmState = {};

    /* [native] timgmemo -- the cross-frame SETTIMG classification memo.
     *
     * WHAT: ResolveTextureSource derives three answers per G_LOADBLOCK/LOADTILE/LOADTLUT --
     * the resolved pointer, TexelsAreNativeRgba16 (a linear scan of gNativeRgba16Ranges) and
     * TimgIsResource (a binary search of the generated asset table) -- for a SETTIMG operand
     * that usually did not change since the last frame. dispatch-progress.md measured that at
     * timgres 1.43 + timgclass 0.93 = 2.36 ms/task, the largest removable block in the census.
     *
     * WHY IT IS AN Interp MEMBER, not a process-wide table next to the bridge's gResolveMemo:
     * the owned interpreter is walked by ONE thread (sInterp is constructed once in
     * gdx_ninterp_job_run and only ever entered from there), so this memo is single-owner BY
     * CONSTRUCTION. That is the same conclusion resolverace reached for the bridge's memo
     * family the expensive way -- and it means this table needs no GdxMemoOwnedHere() latch,
     * no release/acquire pair, and cannot be the seventh member of that bug class.
     *
     * KEY: (resolved pointer, w1, w1hi, requiredBytes) + the bridge's classEpoch. The resolved
     * pointer is an INPUT here, not a cached output -- the memo covers the classification only,
     * and ResolveTimg still runs. That is what makes the key honest AND cross-frame: given p,
     * both classifications are pure functions of the key plus exactly two counted tables
     * (gNativeRgba16Generation and the venue base) and read no segment state at all.
     *
     * Memoising the POINTER too was tried first and measured: it has to be keyed on the
     * adapter's mSegGen, and mSegGen folds gSegments[1], the double-buffered GfxPool the game
     * rewrites every frame, so the epoch turned over once per task and the hit rate was 3.7 %
     * (hit=256 miss=6592 per 64-task window). The bridge's own gWideStubMemo has the same key
     * and the same result -- 0 hits, 6592 misses. See loadblock-progress.md S2.
     *
     * Direct-mapped, no eviction policy: a collision is a miss, which is correct, just slower.
     * ~128 distinct operands per crowd task against 256 slots. */
    struct TimgClassEntry {
        uint32_t w1;
        uint32_t w1hi;
        uint32_t required;
        uint32_t epoch;
        uintptr_t addr;
        uint8_t native_rgba16;
        uint8_t immutable;
        uint8_t filled;
    };
    static constexpr unsigned kTimgMemoSlots = 256; /* power of two: the index is a mask */
    TimgClassEntry mTimgClassMemo[kTimgMemoSlots];

    /* [native] machines_texskip state (see TexSkipOnDl). One record per machine slot. */
    static constexpr int kTexSkipSlots = 30;
    struct TexSetRec {
        uint32_t gen;         /* machine-list generation the record belongs to (0 = none) */
        uint8_t shapeOk;      /* the list is only SETTIMG(w=1, non-CI)/SETTILE/LOADBLOCK/SETTILESIZE/syncs */
        uint8_t setTileMask;  /* tiles the list SETTILEs */
        uint8_t sizeTileMask; /* tiles the list SETTILESIZEs */
        uint8_t captured;     /* tile/lt/span state captured after a real run at `gen` */
        uint8_t nSpans;       /* TMEM spans the list's LOADBLOCKs leave (base, words) */
        uint16_t spanBase[8];
        uint16_t spanWords[8];
        uint64_t wordMask[8]; /* the TMEM words those spans cover (512 bits) */
        Tile tile[8];         /* texture_tile[] as the list leaves it (fields per set/sizeTileMask) */
        LoadedTexture lt[8];  /* LoadedTexAt(tile.tmem_index) as the list leaves it */
    };
    TexSetRec* mTexRecs = nullptr;
    int mTexSkip = 0;
    TexSkipHooks mTexHooks = {};
    bool mTexVfyArm = false;           /* the armed body is a verify (would-skip) site, not a capture */
    uint32_t mTexSkipTasks = 0;        /* receipt cadence */
    const TexSetRec* mTexOv = nullptr; /* open skip window: the record served in place of the list */
    uint8_t mTexOvMask = 0;            /* tiles swapped in for the window */
    Tile mTexOvSaved[8];               /* the live descriptors they replaced */
    const void* mTexOvBody = nullptr;  /* the body the window was opened for */
    TexSetRec* mTexRecArm = nullptr;   /* a real set just ran: capture its state at this body */
    const void* mTexRecArmBody = nullptr;
    TexSkipCounters mTexSkipStats = {};
    uint32_t mTexSkipTripLogs = 0;
    /* gdx_texcache.cpp: an import inside a window (tripwire on a non-immutable source). */
    void TexSkipCheckImport(int tile);
    /* gdx_tmem.cpp / gdx_interp.cpp: a texture-state writer inside an open window. */
    void TexSkipNoteWriter(uint8_t op) {
        if (mTexOv != nullptr) {
            TexSkipTrip(1, op);
        }
    }
    void TexSkipTrip(int kind, uint32_t detail);
    /* gdx_texskip.cpp */
    bool TexSkipScanList(const ListRef& list, TexSetRec& rec);
    bool TexSkipPeekBody(const Frame& f, const void** body);
    int TexSkipChain(const Frame& f, int* chain, uint8_t* chainTiles, int cap);
    void TexSkipBuildPlan(const Frame& f);
    void TexSkipBuildPlanBody(const Frame& f);
    int TexSkipOnDlBody(const void* listPtr);
    uint8_t TexSkipPlanBodyTiles(const ListRef& list, size_t setIdx);
    static constexpr int kChainMaxSets = 32;
    struct TexSkipPlan {
        uint32_t task;          /* mTexSkipTaskSeq it was built in */
        const uint8_t* base;    /* the list it indexes */
        int n;
        uint32_t idx[kChainMaxSets]; /* each set's G_DL index, in stream order */
        int slot[kChainMaxSets];
        uint8_t bodyTiles[kChainMaxSets]; /* tiles the set's body can sample (0xFF unknown) */
    };
    TexSkipPlan mTexPlan = {};
    uint32_t mTexSkipTaskSeq = 1;
    bool TexSkipDlIsSpeck(const void* listPtr);
    bool TexSkipCovered(const TexSetRec& rec, const int* chain, const uint8_t* chainTiles, int n, uint32_t gen);
    bool TexSkipCanSkip(const TexSetRec& rec, const int* chain, const uint8_t* chainTiles, int n, uint8_t bodyTiles,
                        uint32_t gen);
    uint8_t TexSkipInheritedUse() const;
    bool TexSkipLiveTilesClear(const TexSetRec& rec, uint8_t live);
    void TexSkipOpen(const TexSetRec& rec, const void* body);
    void TexSkipClose();
    void TexSkipCapture(TexSetRec& rec);
    void TexSkipVerifyAt(const TexSetRec& rec);
    void TexSkipReceipt();

    /* [native] machines_loadmemo (gdx_setload.cpp) */
    struct LdMemo;
    LdMemo* mLdMemo = nullptr;
    int mLoadMemo = 0;
    bool SetListLoadBlock(const Cmd& c, const Frame& f);
    void SetListLoadServe(const uint8_t* tlAddr, uint8_t imm, uint8_t native, const LoadedTexture& memo,
                          uint32_t readable, uint16_t tmemIndex);

    /* [debug] machsplit (gdx_machsplit.cpp) */
    static constexpr int kMsClasses = 10;
    struct MachSplitWin {
        uint64_t ticks[kMsClasses];
        uint64_t meshTicks[kMsClasses];
        uint32_t calls[kMsClasses];
        uint32_t cmds[kMsClasses];
        uint32_t loads[kMsClasses];
        uint32_t batches[kMsClasses];
        uint64_t resolveTicks[kMsClasses]; /* G_DL / G_MTX operand resolution, side-timed */
        uint64_t texskipTicks, planTicks; /* TexSkipOnDl (incl. its plan) / the plan alone */
        uint32_t texskipCalls, plans;
        uint64_t subTicks[3]; /* inside the speck lists: vtx / tri / other */
        uint32_t subCalls[3];
        uint32_t tasks;
    };
    int mMsOn = 0;
    uint64_t (*mMsMeshTicks)(void) = nullptr;
    uint8_t mMsCat = 0;         /* the [c5cat] category the walk is in (0x61 markers) */
    int mMsDepth = 0;           /* stack depth of the MACH section's commands */
    int mMsClass = -1;          /* the class open, -1 none */
    bool mMsAfterBlend = false; /* the previous section command was SETBLENDCOLOR (highlight) */
    uint64_t mMsT0 = 0, mMsMesh0 = 0;
    uint32_t mMsCmds0 = 0, mMsLoads0 = 0, mMsBatches0 = 0, mMsSkips0 = 0;
    MachSplitWin mMsWin = {};
    void WalkMachSplit();
    void MachSplitMarker(uint8_t cat);
    void MachSplitOpen(const Cmd& c);
    void MachSplitClose();
    void MachSplitReceipt();

    /* [native] dlmemo / deadsetup (gdx_dlmemo.cpp) */
    struct DlMemo;
    DlMemo* mDlMemo = nullptr;
    int mDlMemoLevel = 0;
    struct DeadSet;
    DeadSet* mDs = nullptr;
    int mDeadSetLevel = 0;
    bool ResolveListMemo(const Cmd& c, ListRef& out, bool hitOnly);
    /* entryOut: the memo entry that answered (a hit), else nullptr -- deadsetup's plan cache
     * names the entries its lists came from. */
    bool ResolveListMemoKeyed(const Cmd& c, ListRef& out, bool hitOnly, const GdxDlMemoKey* keyIn,
                              const void** entryOut);
    int DeadSetOnDl(const Cmd& c, Frame& f);
    bool DeadSetBuild(const Cmd& c, Frame& f, const GdxDlMemoKey& key);
    bool DeadSetReuse(Frame& f, const GdxDlMemoKey& key);
    void DeadSetStore(const Frame& f, size_t start, bool pending);
    void DeadSetActivate(Frame& f);
    bool DeadSetMachineList(const void* p) const;
    void DeadSetDecline(const Frame& f, size_t start, bool pending);
    bool DeadSetDeclined(const Frame& f);
    int DeadSetFind(const Cmd& first, bool pending);
    bool DeadSetSameWords(const void* planList, const void* entry) const;
    int DeadSetSlot(const Cmd& first, bool pending);
    bool DeadSetNextIsDl(const Frame& f) const;
    int DeadSetRunList(Frame& f);
    void DeadSetVerify(Frame& f);
    bool DeadSetEligible(const Cmd& c, const Frame& f) const;
    void DeadSetTaskEnd();
    void DeadSetCheckCover(const Cmd& c);
    void DeadSetFree();
    /* gdx_hud_direct.cpp: the TMEM debt ledger, saved and restored around deadsetup's verify. */
    size_t DebtStateSave(void* dst, size_t cap) const;
    void DebtStateLoad(const void* src, size_t n);

    DlHook mDlHook = nullptr;
    void* mDlHookUser = nullptr;
    ListRef mDlHookSub; /* the list offered to mDlHook (Exec OP_DL) */
    NoopHook mNoopHook = nullptr;
    void* mNoopHookUser = nullptr;
    uint32_t mUnknownLogged[8];
    /* menus-s2b members sit last: a member added mid-class moves the offsets every hot loop after it
     * encodes (the race / HUD hot sets change size with no code change). */
    uint8_t mNiMenuSplit = 0;  /* [native] ni_menu_split bits */
    bool mNiInSplit = false;   /* a split run's segments are running (NrrMenuSplit) */
    bool mNiMenuKeep = false;  /* [native] ni_menu_keep */
    bool NrrMenuKeepRefuse(); /* gdx_nrr_menu.inc: a full memo / pool is not cleared for this run */
    struct NiSplitWin {
        uint32_t runs, segs, prog; /* split runs, their segments, the segments that took a program */
        uint32_t keepMemo, keepPool; /* ni_menu_keep: memo / pool clears refused */
    } mNiSplitWin = {};
};

} // namespace gdxn

#endif /* GDX_INTERP_H */
