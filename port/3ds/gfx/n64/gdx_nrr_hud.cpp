/* port/3ds/gfx/n64/gdx_nrr_hud.cpp -- [native] nrr_hud: the NRR HUD (n60, milestone 1).
 * Design: docs/research/native/n60-design.md §4.1. State and receipts:
 * docs/research/native/n60hud-progress.md.
 *
 * This file starts with the INVENTORY census ([native] nrr_hud_census=1): for every HUD-kind
 * 0x5E run it simulates the run's register writes over a scratch copy of the entry registers
 * and logs what a rectangle reads -- the opcode histogram, the distinct (combine, othermode)
 * states at the rectangles, the textures the rectangles sample (format, size, tile extent, the
 * SETTIMG operand's class) and the rectangles' screen box. It changes nothing drawn. */
#include "gdx_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdx3ds_hud_record.h"
#include "gdx3ds_hotcold.h"
#include "gdx_log.h"
#include "gdx3ds_replay.h"
#include "gdx_dlsrc_bridge.h"
#include "../nrr/gdx3ds_nrr.h"
#include "../nrr/gdx3ds_nrr_internal.h" /* n60hud3: gdx3ds_nrr_bk_tex (verify: what a state binds), as nrr_fx */
extern "C" int gdx3ds_stereo_replay_frame; /* n60stereo (gdx3ds_stereo_frame.cpp) */
#include "../nrr/gdx3ds_nrr_crumb.h" /* n60hwdiag */
#include "gdx3ds_cpucut.h"

/* The NRR core lives in 3DS-only objects. The interpreter's host harness compiles this TU without
 * them: there the core entry points are inert stubs (the lever never acts on the host; macOS ld
 * does not resolve undefined weak references, so they are real definitions, host only). */
#ifndef __3DS__
extern "C" {
int gdx3ds_nrr_open(NrrCtx*, void*, int) {
    return 0;
}
void gdx3ds_nrr_close(NrrCtx*) {}
int gdx3ds_nrr_level(int) {
    return 0;
}
const NrrState* NrrStateGet(const NrrStateKey*) {
    return nullptr;
}
uint32_t NrrStateGeneration(void) {
    return 0;
}
void gdx3ds_nrr_state_flush(void) {}
void NrrStateApply(NrrCtx*, const NrrState*) {}
void NrrSetInputs(NrrCtx*, const NrrState*, const float (*)[4], int) {}
void* NrrRingAlloc(NrrCtx*, uint32_t) {
    return nullptr;
}
void NrrDraw2D(NrrCtx*, const float*, uint32_t) {}
void NrrStereoAnchor(NrrCtx*, float) {}
void NrrStereoMenuRow(NrrCtx*, float**, uint32_t*) {}
int gdx3ds_stereo_menu_frame(void) {
    return 0;
}
int gdx3ds_stereo_replay_frame = 0;
int gdx3ds_nrr_bk_tex(uint32_t, NrrBkTex*) {
    return 0;
}
uint32_t gdx3ds_nrr_bk_tex_serial(uint32_t) {
    return 0;
}
uint32_t NrrStateBytes(void) {
    return 8;
}
const NrrState* NrrStateBuildInto(const NrrStateKey*, void*) {
    return nullptr;
}
const NrrState* NrrStateRescissor(const NrrState*, void*, int, int, int, int) {
    return nullptr;
}
void gdxhud_kept_refuse(uint32_t) {}
}
#endif

/* The HUD category's init (gdx3ds_nrr.h "how to add a category"): the HUD draws from its own hook
 * -- the hud_batch capture's 0x5E runs, decoded below -- so it has no snapshot and no marker. */
extern "C" void gdx3ds_nrr_cat_init_hud(void) {}

extern "C" long long gdx_ninterp_now(void);
extern "C" void gdxhud_kept_refuse(uint32_t target); /* nimenu: gdx3ds_hud_capture.c (host: the stub above) */

namespace gdxn {

namespace {

constexpr uint32_t kCensusEvery = 1024; /* a whole frame's runs every this many game frames */
constexpr uint32_t kCensusSigs = 256;   /* distinct run shapes remembered (first sight is logged) */

/* What the census tracks of a tile: the SETTILE shape and the SETTILESIZE extent. */
struct CenTile {
    uint8_t fmt, siz, cms, cmt, masks, maskt, palette;
    uint16_t tmem, line;
    uint16_t uls, ult, lrs, lrt;
};

/* What a load left at a TMEM address: the SETTIMG operand and the load extent. */
struct CenLoad {
    uint16_t tmem;
    uint8_t fmt, siz, used;
    uint32_t w1, w1hi, width, lrs;
};

struct CenSim {
    uint32_t omh, oml;
    uint64_t comb;
    uint32_t timgW0, timgW1, timgW1hi;
    CenTile tile[8];
    CenLoad load[16];
    uint32_t nLoad;
    uint32_t tlutW1, tlutW1hi;
};

struct CenState {
    uint64_t comb;
    uint32_t omh, oml;
};

struct CenTex {
    uint8_t fmt, siz;
    uint16_t w, h;
    uint32_t w1, w1hi;
};

struct CenOut {
    uint16_t ops[256];
    CenState st[6];
    uint32_t nSt;
    CenTex tex[8];
    uint32_t nTex;
    uint32_t texRects, fillRects, primDepthRects, copyRects;
    int32_t x0, y0, x1, y1;
};

/* == Interp::SetCombineWords' packing (mRdp.combine_mode). */
uint64_t PackedCombine(uint32_t w0, uint32_t w1) {
    const uint32_t rgb = C0(w0, 20, 4) | (C1(w1, 28, 4) << 4) | (C0(w0, 15, 5) << 8) | (C1(w1, 15, 3) << 13);
    const uint32_t alpha = C0(w0, 12, 3) | (C1(w1, 12, 3) << 3) | (C0(w0, 9, 3) << 6) | (C1(w1, 9, 3) << 9);
    const uint32_t rgb2 = C0(w0, 5, 4) | (C1(w1, 24, 4) << 4) | (C0(w0, 0, 5) << 8) | (C1(w1, 6, 3) << 13);
    const uint32_t alpha2 = C1(w1, 21, 3) | (C1(w1, 3, 3) << 3) | (C1(w1, 18, 3) << 6) | (C1(w1, 0, 3) << 9);
    return static_cast<uint64_t>(rgb) | (static_cast<uint64_t>(alpha) << 16) | (static_cast<uint64_t>(rgb2) << 28) |
           (static_cast<uint64_t>(alpha2) << 44);
}

uint32_t RunSignature(const GdxHudWord* w, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ (w[i].w0 >> 24)) * 16777619u;
    }
    return h ^ n;
}

void CenseSetTile(CenSim& s, uint32_t w0, uint32_t w1) {
    CenTile& t = s.tile[C1(w1, 24, 3)];
    t.fmt = static_cast<uint8_t>(C0(w0, 21, 3));
    t.siz = static_cast<uint8_t>(C0(w0, 19, 2));
    t.line = static_cast<uint16_t>(C0(w0, 9, 9));
    t.tmem = static_cast<uint16_t>(C0(w0, 0, 9));
    t.palette = static_cast<uint8_t>(C1(w1, 20, 4));
    t.cmt = static_cast<uint8_t>(C1(w1, 18, 2));
    t.maskt = static_cast<uint8_t>(C1(w1, 14, 4));
    t.cms = static_cast<uint8_t>(C1(w1, 8, 2));
    t.masks = static_cast<uint8_t>(C1(w1, 4, 4));
}

void CenseSetTileSize(CenSim& s, uint32_t w0, uint32_t w1) {
    CenTile& t = s.tile[C1(w1, 24, 3)];
    t.uls = static_cast<uint16_t>(C0(w0, 12, 12));
    t.ult = static_cast<uint16_t>(C0(w0, 0, 12));
    t.lrs = static_cast<uint16_t>(C1(w1, 12, 12));
    t.lrt = static_cast<uint16_t>(C1(w1, 0, 12));
}

void CenseLoad(CenSim& s, uint32_t w1) {
    const CenTile& t = s.tile[C1(w1, 24, 3)];
    CenLoad* slot = nullptr;
    for (uint32_t i = 0; i < s.nLoad; i++) {
        if (s.load[i].tmem == t.tmem) {
            slot = &s.load[i];
        }
    }
    if (slot == nullptr && s.nLoad < 16) {
        slot = &s.load[s.nLoad++];
    }
    if (slot == nullptr) {
        return;
    }
    slot->tmem = t.tmem;
    slot->fmt = static_cast<uint8_t>(C0(s.timgW0, 21, 3));
    slot->siz = static_cast<uint8_t>(C0(s.timgW0, 19, 2));
    slot->width = C0(s.timgW0, 0, 12) + 1;
    slot->w1 = s.timgW1;
    slot->w1hi = s.timgW1hi;
    slot->lrs = C1(w1, 12, 12);
    slot->used = 1;
}

void CenseNoteState(CenOut& o, const CenSim& s) {
    for (uint32_t i = 0; i < o.nSt; i++) {
        if (o.st[i].comb == s.comb && o.st[i].omh == s.omh && o.st[i].oml == s.oml) {
            return;
        }
    }
    if (o.nSt < 6) {
        o.st[o.nSt].comb = s.comb;
        o.st[o.nSt].omh = s.omh;
        o.st[o.nSt].oml = s.oml;
    }
    o.nSt++;
}

void CenseNoteTex(CenOut& o, const CenSim& s, uint32_t tileIdx) {
    const CenTile& t = s.tile[tileIdx & 7u];
    CenTex x;
    memset(&x, 0, sizeof(x));
    x.fmt = t.fmt;
    x.siz = t.siz;
    x.w = static_cast<uint16_t>((t.lrs - t.uls + 4) / 4);
    x.h = static_cast<uint16_t>((t.lrt - t.ult + 4) / 4);
    for (uint32_t i = 0; i < s.nLoad; i++) {
        if (s.load[i].tmem == t.tmem) {
            x.w1 = s.load[i].w1;
            x.w1hi = s.load[i].w1hi;
        }
    }
    for (uint32_t i = 0; i < o.nTex && i < 8; i++) {
        if (memcmp(&o.tex[i], &x, sizeof(x)) == 0) {
            return;
        }
    }
    if (o.nTex < 8) {
        o.tex[o.nTex] = x;
    }
    o.nTex++;
}

void CenseNoteBox(CenOut& o, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    o.x0 = ulx < o.x0 ? ulx : o.x0;
    o.y0 = uly < o.y0 ? uly : o.y0;
    o.x1 = lrx > o.x1 ? lrx : o.x1;
    o.y1 = lry > o.y1 ? lry : o.y1;
}

void CenseRect(CenOut& o, const CenSim& s, uint32_t w0, uint32_t w1, bool fill) {
    CenseNoteState(o, s);
    if ((s.oml & ZS_PRIM) != 0) {
        o.primDepthRects++;
    }
    if ((s.omh & (3u << MDSFT_CYCLETYPE)) == CYC_COPY) {
        o.copyRects++;
    }
    CenseNoteBox(o, static_cast<int32_t>(C1(w1, 12, 12)), static_cast<int32_t>(C1(w1, 0, 12)),
                 static_cast<int32_t>(C0(w0, 12, 12)), static_cast<int32_t>(C0(w0, 0, 12)));
    if (fill) {
        o.fillRects++;
        return;
    }
    o.texRects++;
    CenseNoteTex(o, s, C1(w1, 24, 3));
}

void CenseWord(CenSim& s, CenOut& o, uint32_t w0, uint32_t w1, uint32_t w1hi) {
    const uint8_t op = Opcode(w0);
    o.ops[op]++;
    switch (op) {
        case OP_SETTIMG:
            s.timgW0 = w0;
            s.timgW1 = w1;
            s.timgW1hi = w1hi;
            break;
        case OP_SETTILE: CenseSetTile(s, w0, w1); break;
        case OP_SETTILESIZE: CenseSetTileSize(s, w0, w1); break;
        case OP_LOADBLOCK:
        case OP_LOADTILE: CenseLoad(s, w1); break;
        case OP_LOADTLUT:
            s.tlutW1 = s.timgW1;
            s.tlutW1hi = s.timgW1hi;
            break;
        case OP_SETCOMBINE: s.comb = PackedCombine(w0, w1); break;
        case OP_SETOTHERMODE_H: {
            const uint32_t len = C0(w0, 0, 8) + 1, sft = 32 - C0(w0, 8, 8) - len;
            const uint32_t mask = ((len >= 32 ? 0u : (1u << len)) - 1u) << sft;
            s.omh = (s.omh & ~mask) | (w1 & mask);
            break;
        }
        case OP_SETOTHERMODE_L: {
            const uint32_t len = C0(w0, 0, 8) + 1, sft = 32 - C0(w0, 8, 8) - len;
            const uint32_t mask = ((len >= 32 ? 0u : (1u << len)) - 1u) << sft;
            s.oml = (s.oml & ~mask) | (w1 & mask);
            break;
        }
        case OP_RDPSETOTHERMODE:
            s.omh = C0(w0, 0, 24);
            s.oml = w1;
            break;
        case OP_TEXRECT:
        case OP_TEXRECTFLIP: CenseRect(o, s, w0, w1, false); break;
        case OP_FILLRECT: CenseRect(o, s, w0, w1, true); break;
        default: break;
    }
}

void CenseInit(CenSim& s, CenOut& o, const Rdp& rdp) {
    memset(&s, 0, sizeof(s));
    memset(&o, 0, sizeof(o));
    s.omh = rdp.other_mode_h;
    s.oml = rdp.other_mode_l;
    s.comb = rdp.combine_mode;
    s.timgW0 = rdp.texture_to_load.cmd.w0;
    s.timgW1 = rdp.texture_to_load.cmd.w1;
    s.timgW1hi = rdp.texture_to_load.cmd.w1hi;
    for (int t = 0; t < 8; t++) {
        const Tile& r = rdp.texture_tile[t];
        s.tile[t].fmt = r.fmt;
        s.tile[t].siz = r.siz;
        s.tile[t].tmem = r.tmem;
        s.tile[t].line = static_cast<uint16_t>(r.line_size_bytes / 8u);
        s.tile[t].uls = static_cast<uint16_t>(r.uls);
        s.tile[t].ult = static_cast<uint16_t>(r.ult);
        s.tile[t].lrs = static_cast<uint16_t>(r.lrs);
        s.tile[t].lrt = static_cast<uint16_t>(r.lrt);
    }
    o.x0 = o.y0 = 0x7FFFFFFF;
    o.x1 = o.y1 = -0x7FFFFFFF;
}

/* "FD:3 F5:6 ..." for every opcode the run carries. */
int FormatOps(char* buf, size_t cap, const CenOut& o) {
    int n = 0;
    buf[0] = '\0';
    for (int op = 0; op < 256 && n >= 0 && static_cast<size_t>(n) < cap; op++) {
        if (o.ops[op] != 0) {
            n += snprintf(buf + n, cap - static_cast<size_t>(n), "%s%02X:%u", n != 0 ? "," : "", op,
                          static_cast<unsigned>(o.ops[op]));
        }
    }
    return n;
}

int FormatStates(char* buf, size_t cap, const CenOut& o) {
    int n = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < o.nSt && i < 6 && n >= 0 && static_cast<size_t>(n) < cap; i++) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n), "%s%08lx%08lx/%08lx/%08lx", i != 0 ? "," : "",
                      static_cast<unsigned long>(o.st[i].comb >> 32), static_cast<unsigned long>(o.st[i].comb),
                      static_cast<unsigned long>(o.st[i].omh), static_cast<unsigned long>(o.st[i].oml));
    }
    return n;
}

int FormatTex(char* buf, size_t cap, const CenOut& o) {
    static const char* const kFmt[8] = { "RGBA", "YUV", "CI", "IA", "I", "f5", "f6", "f7" };
    static const char* const kSiz[4] = { "4", "8", "16", "32" };
    int n = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < o.nTex && i < 8 && n >= 0 && static_cast<size_t>(n) < cap; i++) {
        const CenTex& x = o.tex[i];
        const char cls = x.w1hi == kHostTag ? 'H' : 'S';
        n += snprintf(buf + n, cap - static_cast<size_t>(n), "%s%s%s:%ux%u@%c%08lx", i != 0 ? "," : "",
                      kFmt[x.fmt & 7u], kSiz[x.siz & 3u], static_cast<unsigned>(x.w), static_cast<unsigned>(x.h),
                      cls, static_cast<unsigned long>(x.w1));
    }
    return n;
}

} // namespace


/* =========================================================================================
 * The NRR HUD decoder ([native] nrr=1 nrr_hud=1).
 *
 * ONE loop over a HUD-kind run's words (NrrHudHotRun):
 *   - register words go through the walker's own handler (Exec, the same code the kept setup
 *     lists between two runs execute), so the registers are LIVE and exact after every word --
 *     there is no write-back to compile, and a kept G_DL between two runs composes with them
 *     exactly as it does with the replay;
 *   - a host-operand LOADBLOCK is VIRTUAL: it records which source now sits at which TMEM words
 *     (the texture identity a rectangle's state key carries) and is queued as PENDING; the
 *     interpreter's TMEM is brought up to date only if an interpreter reader needs those words
 *     (cold; NrrTmemRead) or an interpreter writer only partly overwrites them (NrrTmemWrite);
 *   - a TEXRECT / FILLRECT computes its state key (combine, othermode, the tile's shape, the
 *     texture identity, the scissor, the blend alpha), finds its memoised STATE BLOCK, and writes
 *     its four corners -- packed with the direct path's expressions -- as two triangles into the
 *     NRR core's ring; batches break on a state block change (NrrStateApply) or a change of a
 *     register the batch's TexEnv constants read, and drain through the core's 2D draw.
 * A key seen for the first time takes the STOCK rectangle (TMEM settled first) and the block is
 * captured from what that rectangle did, then self-checked: the block's packing must reproduce
 * the stock rectangle's floats bit for bit, or the key stays on the stock path. Words the
 * decoder does not model (triangles, a depth-clear FILLRECT) run through Exec with TMEM settled
 * and citro3d handed back first. Both stock arms count as `fallback` on the receipt.
 * ========================================================================================= */

namespace {

constexpr uint32_t kBlocks = 256; /* state-block memo (open addressed, power of two) */
constexpr uint32_t kProbe = 8;
/* menus-s2 [native] ni_menu: the memo's probe length on a 2D screen. A menu frame captures 100-150
 * blocks (one per glyph texture); at that load a linear probe of 8 meets a full chain every ~16
 * frames on the main menu, and the clear it falls back to (every block, so every program) turned the
 * whole screen into per-word decodes with the interpreter's batch open -- no program could be
 * recorded, every run tripped to the stock replay (`vol=`), ~50 texture imports a frame. A longer
 * probe only changes which slot a block sits in (the lookups are cold): the output is the same. */
constexpr uint32_t kProbeMenu = 32;
constexpr uint32_t kScVars = 128; /* [native] nrr_hud_late bit 1: scissor variants kept (64 sets of 2) */
constexpr uint32_t kScStates = 48; /* their core states (a ring; a reused one retires its variant) */
constexpr uint32_t kScSibs = 256;  /* blocks indexed by their key without the scissor (direct-mapped) */
constexpr int kLateScissor = 1;  /* [native] nrr_hud_late bit 1: a program's rectangles take the live scissor */
constexpr int kLatePitch = 2;    /* bit 2: a wide load's pitch is checked when it runs, not an entry dependency */
constexpr int kLateFlush = 4;    /* bit 4: an interpreter batch open at a run's start (the countdown sign's
                                  * triangles) is drawn first, as NrrHudOpen would, so the run's program can run */
constexpr uint32_t kSlots = 8;    /* TMEM identity ranges */
constexpr uint32_t kPend = 32;    /* virtual loads not yet in the interpreter's TMEM */
constexpr size_t kStride = GDX3DS_VBOPACK_STRIDE;
constexpr uint32_t kQuadFloats = 6u * GDX3DS_VBOPACK_STRIDE;

/* == gdx_tri.cpp FminExact */
inline float NrrFmin(float x, float y) {
    if (x != x) {
        return y;
    }
    if (y != y) {
        return x;
    }
    return x < y ? x : y;
}

/* == gdx_tmem.cpp BytesForTexels */
inline uint32_t NrrTexelBytes(uint32_t texels, uint8_t siz) {
    switch (siz) {
        case IM_SIZ_4b: return (texels + 1u) / 2u;
        case IM_SIZ_8b: return texels;
        case IM_SIZ_16b: return texels * 2u;
        default: return texels * 4u;
    }
}

/* == SpTri's gdxInput lambda (gdx_tri.cpp; gdx_hud_direct.cpp GdxInput): combiner input j of
 * channel k at vertex vi of a triangle whose first vertex is v1. Cold: a block whose vertex
 * colour comes from anything but a register / the shade / a constant packs through it. */
struct NrrIn {
    Rgba c;
    bool isShade;
};

GDX_COLD NrrIn NrrInputGeneric(const Rdp& rdp, const Combiner* comb, int j, int k, const LoadedVertex* vi,
                               const LoadedVertex* v1) {
    Rgba tmp = { 0, 0, 0, 0 };
    bool isShade = false;
    switch (comb->shader_input_mapping[k][j]) {
        case CCMUX_PRIMITIVE: tmp = rdp.prim_color; break;
        case CCMUX_SHADE: tmp = vi->color; isShade = true; break;
        case CCMUX_SHADE_ALPHA: tmp.r = tmp.g = tmp.b = tmp.a = vi->color.a; break;
        case CCMUX_ENVIRONMENT: tmp = rdp.env_color; break;
        case CCMUX_PRIMITIVE_ALPHA: tmp.r = tmp.g = tmp.b = rdp.prim_color.a; break;
        case CCMUX_ENV_ALPHA: tmp.r = tmp.g = tmp.b = rdp.env_color.a; break;
        case CCMUX_PRIM_LOD_FRAC: tmp.r = tmp.g = tmp.b = rdp.prim_lod_fraction; break;
        case CCMUX_LOD_FRACTION: {
            if (rdp.other_mode_l & (1u << MDSFT_TEXTLOD)) {
                float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                if (distance_frac < 0.0f) {
                    distance_frac = 0.0f;
                }
                if (distance_frac > 1.0f) {
                    distance_frac = 1.0f;
                }
                tmp.r = tmp.g = tmp.b = tmp.a = static_cast<uint8_t>(distance_frac * 255.0f);
            } else {
                tmp.r = tmp.g = tmp.b = tmp.a = 255;
            }
            break;
        }
        case CCMUX_KEY_CENTER: tmp = rdp.key_center; break;
        case CCMUX_KEY_SCALE: tmp = rdp.key_scale; break;
        case CCMUX_CONVERT_K4: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(rdp.convert_k[4]); break;
        case CCMUX_CONVERT_K5: tmp.r = tmp.g = tmp.b = static_cast<uint8_t>(rdp.convert_k[5]); break;
        default: break;
    }
    if (k == 1 && comb->shader_input_mapping[k][j] == ACMUX_PRIM_LOD_FRAC) {
        tmp = Rgba{ 0, 0, 0, rdp.prim_lod_fraction };
        isShade = false;
    }
    return NrrIn{ tmp, isShade };
}

/* The forced combines of the COPY-cycle TEXRECT (DpTextureRectangle) and the FILL-cycle
 * FILLRECT (DpFillRectangle), in mRdp.combine_mode's packing. */
constexpr uint64_t kCopyCombine = static_cast<uint64_t>((CCMUX_TEXEL0 & 7u) << 13) |
                                  (static_cast<uint64_t>((ACMUX_TEXEL0 & 7u) << 9) << 16);
constexpr uint64_t kFillCombine = static_cast<uint64_t>((CCMUX_SHADE & 7u) << 13) |
                                  (static_cast<uint64_t>((ACMUX_SHADE & 7u) << 9) << 16);

/* Constant corner colours for the resolved selectors (mByteToUnit[255] == 1.0f exactly). */
const Rgba kRgbaOne = { 255, 255, 255, 255 };
const Rgba kRgbaZero = { 0, 0, 0, 0 };

enum : uint8_t {
    NRR_CONST_PRIM = 1, /* prim colour / alpha / lod fraction feeds a TexEnv constant */
    NRR_CONST_ENV = 2,
    NRR_CONST_SHADE = 4 /* the first corner's vertex colour does */
};

/* A rectangle's two SpTri calls, (ul, ll, ur) then (ll, lr, ur): the ring slot each corner
 * (ul=0 ll=1 lr=2 ur=3) is packed into, and the two slots that repeat a corner. */
const uint8_t kCornerSlot[4] = { 0, 1, 4, 2 };

inline uint32_t NrrRgbaWord(const Rgba& c) {
    uint32_t w;
    memcpy(&w, &c, sizeof(w));
    return w;
}

} // namespace

namespace {

/* The hot loops' small constant trip counts (corners, key words, slots) stay loops: complete
 * unrolling multiplies the bytes the loop fetches for no work saved (a per-loop source directive,
 * like noinline; the build's codegen flags are untouched). */
#define NRR_NO_UNROLL _Pragma("GCC unroll 1")

/* A 32-bit view of the interpreter's register structs: a compiled register word writes them as
 * masked words (the handlers write bytes, halves and words of the same memory). */
typedef uint32_t __attribute__((may_alias)) NrrWord32;

constexpr uint32_t kKeyWords = 23;  /* NrrKey */
constexpr uint32_t kRegFx = 1024;   /* compiled register words (open addressed, power of two) */
constexpr uint32_t kRegProbe = 4;
constexpr uint32_t kFxStores = 8;   /* masked stores per compiled word (SETTIMG needs 7) */
constexpr uint8_t kFxExecOnly = 0xFF; /* NrrRegFx::n of a word that always runs its handler */
constexpr uint8_t kFxSeen = 0xFE;     /* NrrRegFx::n of a word seen once: compiled at its second sight */
constexpr uint32_t kClasses = 64;   /* interned batch classes (NrrInternClass) */
constexpr uint32_t kNoClass = 0xFFFFFFFFu;
constexpr uint32_t kFxScratch = 512; /* bytes of register memory a compile snapshots (NrrRegRegions) */
/* run programs (NrrHudHotProg) */
constexpr uint32_t kProgBits = 8;
constexpr uint32_t kProgs = 1u << kProgBits; /* open addressed by the skeleton hash (NrrProgSlot) */
constexpr uint32_t kProgProbe = 8;      /* programs per skeleton (4P: one per view's entry state) */
constexpr uint32_t kProgMaxWords = 1024; /* longer runs decode per word (the finish screen's text field is 840) */
constexpr uint32_t kRecWords = 8192;     /* one program's ops while it is recorded */
constexpr uint32_t kPoolWords = 32768;  /* every program's skeleton, entry snapshot and ops */
constexpr uint32_t kSnapMax = 160;      /* entry-snapshot words (NrrSnapInit) */
constexpr uint32_t kRecMiss = 256;      /* NrrNoteNoProg's per-hash counters */
constexpr uint32_t kLdWords = 5;        /* an LD op's payload: SETTIMG w0 w1, LOADBLOCK w0 w1, lo | hi << 16 */
constexpr uint32_t kRtWords = 5;
constexpr uint32_t kCoreFlushEvery = 64; /* tasks between two HUD flushes of the core's state table */
/* n60hudx [native] nrr_hud_rr: run replay */
constexpr uint32_t kRrMaxRects = 64;      /* a run of more rectangles is not recorded */
constexpr uint32_t kRrBigRects = 8;       /* lever bit 1: runs of at least this many (the portrait column) */
constexpr uint32_t kRrPoolWords = 49152;  /* the records' arena (192 KB, allocated with the lever) */
constexpr uint32_t kRrSnap = 12;          /* entry words a record is checked on besides its run's words */
constexpr uint32_t kRrHead = 6 + kRrSnap; /* n, memo epoch, state generation, texture epoch, ops, rects, snapshot */
constexpr uint32_t kRrNoReplay = 1u << 16;
constexpr uint32_t kRrSeen = 1u << 17;    /* the program executed once: a capture may follow */
constexpr uint32_t kRrMissShift = 18;
constexpr uint32_t kRrCapsPerTask = 2;    /* captures a task may make (a screen of new runs records over tasks) */
constexpr uint32_t kSlCache = 16;         /* n60hudx: compiled setup lists the per-word decoder reuses */
constexpr uint32_t kSlCacheWords = 128;
enum : uint32_t { kRrNs = 1, kRrLd = 2, kRrRt = 3, kRrFl = 4, kRrSl = 7 };
constexpr uint32_t kSlMaxCmds = 32;   /* n60hudx: an SL list's commands through its terminator */
constexpr uint32_t kSlPayMax = 4u + 4u * kSlMaxCmds + 2u * kSlMaxCmds * (1u + 3u * 8u); /* + ST groups */
constexpr uint32_t kOwnStates = 24;   /* the HUD's own core states when the core table is full (NrrOwnState) */        /* an RT op's payload: the block, the texture identity it was keyed on */
/* A wide load (SETTIMG width > 1, [native] nrr_hud_wide) records its line pitch -- DpLoadBlock's
 * decision under the tiles of the moment (LoadBlockLineBytes) -- in its SETTIMG word: the opcode
 * byte and the unused bits [18:12] carry it (15 bits), the fields stay. So the record names what
 * the load put in TMEM (the texture identity) and NrrMaterializeAt hands the pitch back. */
constexpr uint32_t kTimgFields = 0x00F80FFFu; /* SETTIMG's fmt, siz, width - 1 */
constexpr uint32_t kWideLineMax = 0x7FFFu;
constexpr int kWideLoads = 1;      /* [native] nrr_hud_wide bit 1: virtual SETTIMG width > 1 loads */
constexpr int kWidePageStates = 2; /* bit 2: atlas views' core states keyed by page (NrrStateTex) */
constexpr int kWideSegLoads = 4;   /* bit 4: virtual loads of a segmented operand, resolved at record (NrrSegResolve) */
constexpr uint32_t kSegResolved = 0x80u; /* the LOADBLOCK w0 opcode byte of a resolved segmented record */
constexpr uint32_t kSegImmutable = 0x01u;
constexpr uint32_t kSegNative = 0x02u;
inline bool NrrSegResolved(const uint32_t* r) {
    return (r[2] >> 24) != OP_LOADBLOCK;
}
inline uint32_t NrrWideTimg(uint32_t w0, uint32_t line) {
    return (w0 & kTimgFields) | ((line & 0x7Fu) << 12) | ((line >> 7) << 24);
}
inline uint32_t NrrWideLine(uint32_t r0) {
    return ((r0 >> 12) & 0x7Fu) | ((r0 >> 24) << 7);
}
inline uint32_t NrrWideTimgWord(uint32_t r0) {
    return (r0 & kTimgFields) | (static_cast<uint32_t>(OP_SETTIMG) << 24);
}
enum : uint32_t {
    kOpSt = 1, /* | n << 4 | arm | why << 9 | word << 16; the word (w0 w1 w1hi), n x (dst, mask, val) */
    kOpLd = 2, /* | word << 16; kLdWords */
    kOpRt = 3, /* | tile << 8 | word << 16; the block, the identity at the tile's TMEM address (4 words) */
    kOpFl = 4, /* | word << 16; the block */
    kOpEx = 5, /* | word << 16 */
    kOpCol = 6, /* | why << 9 | word << 16; the colour word, the lod-fraction word (or 0), its shift */
    kOpSl = 7,  /* n60hudx nrr_hud_kept | word << 16; a G_DL to a static setup list: the list's ref and words,
                 * then its register words' stores (NrrSlCompile's payload) */
    kOpArm = 1u << 8,
    kOpColPd = 1u << 14 /* o3ds-cpucuts nrr_hud_pd: a COL op of a SETPRIMDEPTH -- payload {the prim
                         * depth's register word, 0, its shift}; the depth is w1's (z & 0x7FFF) */
};

/* n60hudx: an SL payload's verbatim list words (nCmd commands of its stride) */
inline uint32_t NrrSlRawWords(const uint32_t* pay) {
    return (pay[3] & 0xFFFFu) * ((pay[2] & 0xFFu) / 4u);
}

inline bool NrrRectEq(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

} // namespace

/* A rectangle's state key: every register its state block is a function of, as plain words (no
 * padding of its own; the tile's bytes are copied whole, padding included, which can only split a
 * key, never merge two). Compared word by word. The texture identity is the load's own words (a
 * virtual load is a host-operand G_LOADBLOCK form: the source pointer is its w1).
 *   [0..1] combine (effective: the COPY / FILL forced combines folded in)
 *   [2..3] other mode h (effective: COPY's point filter folded in), l
 *   [4..6] the scissor (Rect: x | y, width, height)
 *   [7]    blend alpha | grayscale << 8 | fill << 9 | tile << 12
 *   [8..16] the tile (struct Tile, 36 bytes; zero for a FILLRECT)
 *   [17..20] SETTIMG w0 / w1, LOADBLOCK w0 / w1 of the load at the tile's TMEM (0 = none)
 *   [21..22] the TLUT operand of each palette half (CI only) */
struct Interp::NrrKey {
    uint32_t w[kKeyWords];
};

/* What ResolveDrawState resolved for a key (the core's state block), plus what the packing reads. */
struct Interp::NrrBlock {
    NrrKey key;
    uint8_t used, stock, usesTex, copy, coldColour;
    uint8_t depth, decal, alpha;
    uint8_t smpLinear, smpCms, smpCmt;
    uint8_t imm, cls;
    uint8_t constRegs; /* NRR_CONST_*: the registers the batch's TexEnv constants read */
    uint8_t auxExotic; /* an input reads a register beyond prim / env / shade / lod fraction (HudPackAux) */
    uint8_t auxN;      /* HudPackAux's numInputs */
    int16_t slot;
    uint32_t texId;
    uint32_t slotGen; /* mNrrSlotGen[slot] at capture: the slot still holds this texture */
    Rect scissor;
    DrawState ds;
    Gdx3dsVboPackParams params; /* the stock rectangle's batch parameters (texture fields included) */
    NrrBlockUv uv;
    const Rgba* colSrc;        /* rgb: a register or a constant; nullptr = the corner's vertex colour */
    const Rgba* alphaSrc;      /* alpha: likewise */
    float z;
    float ySign; /* -1 under an inverted clip, else 1: y = (1 - e / h) * ySign (exact) */
    const uint8_t* srcAddr;
    uint32_t srcBytes;
    uint64_t stamp[2];
    uint32_t checkedSeq;
    uint32_t ver;           /* n60intro: NrrHud::blockVer at its capture (a scissor variant's base check) */
    const NrrState* st;     /* the NRR core's state block (gdx3ds_nrr.h) */
    const NrrState* stNoSc; /* the same with the scissor off (see NrrHudHotReserve) */
    uint32_t stGen;
    /* the batch class (an interned NrrStateKey): the state key with an atlas view's texture
     * replaced by its page -- views on one page bind the same C3D_Tex, so their rectangles share
     * a draw ([trectbatch]) */
    uint32_t batchCls, batchClsNoSc;
    NrrStateKey batchKey;
    NrrStateKey batchKeyNoSc;
    /* HudPackAux, resolved: per input and channel, the byte it reads -- prim r g b a 0..3, env 4..7,
     * the first corner's colour 8..11, lod fraction 12, zero 13 -- or 0xFF for 1.0 (no alpha) */
    uint8_t auxSel[7][4];
};

/* One register word, compiled (cold, NrrRegCompile): the masked 32-bit stores its handler makes
 * into the live registers, and what the handler does when they change something. */
struct Interp::NrrRegFx {
    uint32_t w0, w1, w1hi; /* the word: the key */
    uint8_t used;
    uint8_t n;     /* stores; kFxExecOnly = the handler runs every time (cold) */
    uint8_t arm;   /* a change arms textures_changed (DpSetTile / DpSetTileSize's value gate) */
    uint8_t why;   /* a change counts this (empty) flush, as the handler's NoteRegChange / Flush */
    uint8_t dirty; /* the word moves an input of a rectangle's key */
    NrrWord32* dst[kFxStores];
    uint32_t mask[kFxStores];
    uint32_t val[kFxStores];
};

/* A run's program (see the hot loop's comment): offsets into the pool. */
struct Interp::NrrProg {
    uint32_t hash; /* NrrHudHotHash */
    uint32_t n;    /* the run's words */
    uint32_t deps; /* nDeps x (word, mask, value): the entry registers the run reads before it writes them */
    uint32_t nDeps;
    uint32_t ops;
    uint32_t opWords;
    /* n60hudx [native] nrr_hud_rr: the run's replay record (arena offset + 1, 0 none); rrInfo: its
     * rectangles (low 16 bits), kRrNoReplay (an op a replay cannot carry), the records that missed */
    uint32_t rr;
    uint32_t rrInfo;
};

/* [native] nrr_hud_memo: one program rectangle's last output (NrrHudHotMemo). key = the op (tag),
 * its block, the memo epoch, the rectangle's words (w0, w1, the two trailers' w1), the cycle type,
 * the four corners' colour and alpha sources (8 words). */
struct Interp::NrrRectMemo {
    uint32_t key[kNrrMemoKeyWords];
    float out[kQuadFloats];
};
constexpr uint32_t kRectMemos = 128; /* direct-mapped by the op address, power of two */

struct Interp::NrrHud {
    /* -- hot scalars first: small offsets from the one base pointer -- */
    /* run state */
    const GdxHudWord* runWords; /* the run, for Exec (NrrExecWord) */
    uint32_t runN;
    bool dirty; /* the per-word decoder: a key input moved since the last rectangle */
    bool drew;
    bool open;  /* the hand-off with the NRR core (open from the first NRR draw of a HUD region to its close) */
    bool materializing;
    uint8_t lastWho; /* the last rectangle's tile, or 0x80 for a FILLRECT */
    uint8_t batchConst;
    bool simLive, simVpSet, simScSet, simLastVp, simVpDefault;
    bool xFull, xAdj;
    NrrBlock* last; /* the per-word decoder: the last rectangle's block, valid while no key input moved */
    const NrrState* batchSt;    /* the state of the open batch (nullptr = none) */
    const NrrBlock* batchBlock; /* the block whose rectangle opened it */
    uint32_t batchCls;          /* its class (kNoClass = none) */
    float* batchEnd;
    float* batchStart;
    uint32_t batchVerts;
    uint32_t batchPrim, batchEnv, batchShade; /* the TexEnv-constant registers at the batch's first corner */
    /* What the stock path's viewport / scissor calls would have left, rectangle by rectangle: a
     * rectangle sets the default viewport (which turns the GPU scissor OFF on this backend) and
     * then its scissor only if it differs from the last one set -- so a HUD rectangle after a
     * split-screen view's 3D is drawn unscissored until the next scissor change. NRR draws with
     * the same effective scissor and leaves the backend where the stock path would have. */
    Rect simVp, simSc, defaultVp;
    /* the corner mapping (NrrXMode) */
    float aspect, x4HalfW, y4HalfH;
    float xMul;       /* AdjXForAspectRatio as x * xMul / xDiv: 4/3 and the aspect, or 1 and 1 */
    float xyHalf[2];  /* 4 * half the native width, height */
    float xyDiv[2];   /* xDiv, 1 */
    uint32_t seq;     /* tasks */
    unsigned long atlasEpoch;
    const unsigned long* atlasPtr; /* the backend's atlas epoch counter (Backend::AtlasEpochPtr) */
    uint32_t envSeq;  /* the task whose config the cold check last compared */
    uint32_t envEgm;  /* the extra geometry mode the corner mapping was computed for */
    uint32_t nPend;
    uint32_t progEpoch; /* bumped when every program goes (a memo clear, a new config) */
    uint32_t nSnap;
    int clock;
    NrrRectMemo* memo; /* [native] nrr_hud_memo: kRectMemos entries, nullptr = off */
    uint32_t memoEpoch; /* bumped by a program clear, a block capture, a corner-mapping change */
    uint32_t memoTaskRects; /* memo lookups this task: past kRectMemos the table would only thrash */
    NrrCtx ctx;
    /* receipt window */
    struct Win {
        uint32_t runs, words, rects, fast, stockRects, interp, vloads, rloads, mats, matLoads, builds, selfBad,
            stockKeys, clears, partial, staleRead, draws, regHot, regCold, regBuilds, auxCold, decRuns, progEx,
            progAbandons, progStores, progMiss, progDepMiss, progNoHash, progClears, recFails, ownFull, coreFlushes,
            memoHits, memoMisses, memoSkips, memoRects, lateFlushes, scVarHits, scVarBuilds, scVarTable, scVarPromotes, pitchMiss,
            slHits, slMiss, slWalks, slBuilds, slDecs, rrHits, rrGateMiss, rrResume, rrRecs, rrCapFails, rrVfyOk,
            rrVfyBad, rrRects;
        uint64_t ticks;
        uint64_t ph[4];
        uint32_t phN[4];
    } w;
    /* what the NRR knows sits at which TMEM words: the load's own words */
    struct Slot {
        uint16_t lo, hi; /* lo == hi: free */
        uint32_t src[4];
    } slots[kSlots];
    uint32_t tlut[2]; /* the TLUT operand (host w1) each palette half was loaded from, 0 = unknown */
    /* virtual loads the interpreter's TMEM does not hold yet, oldest first */
    struct Pend {
        uint32_t timgW0, timgW1; /* its SETTIMG (a host operand: w1hi is kHostTag) */
        uint32_t lw0, lw1;       /* its LOADBLOCK (the load tile in lw1) */
        uint16_t lo, hi; /* TMEM words; lo is the load tile's tmem_index (all DpLoadBlock reads of it);
                          * lo == hi: dead (a later pending load covers it whole) */
    } pend[kPend];
    /* the entry snapshot: these words under these masks (NrrSnapInit) */
    const NrrWord32* snapPtr[kSnapMax];
    uint32_t snapMask[kSnapMax];
    /* run programs: the table, the pool */
    NrrProg progs[kProgs];
    uint32_t nProgs, poolUsed;
    /* -- cold below -- */
    Frame frame;
    uint32_t sesFallback, sesSelfBad;
    uint32_t selfBadLogs;
    /* census ([native] nrr_hud_census) */
    uint32_t sigs[kCensusSigs];
    uint32_t nSigs;
    uint32_t lastFrame;
    uint32_t runInFrame;
    /* the state-block memo */
    uint32_t nBlocks;
    NrrStateKey classes[kClasses]; /* interned batch classes; index = NrrBlock::batchCls */
    uint32_t nClasses, nUnique;
    /* the HUD's own core states (NrrOwnState: the core's table was full) */
    const NrrState* own[kOwnStates];
    void* ownMem[kOwnStates];
    NrrStateKey ownKey[kOwnStates];
    uint32_t nOwn, ownGen;
    bool coreFlushed;
    uint32_t coreFlushSeq; /* the task of the HUD's last core-table flush (NrrCoreTableFlush) */
    /* the compiled register words, and the frame config their stores were computed under */
    uint32_t nRegs;
    uint32_t env[8]; /* what DpSetScissor's mapping reads (NrrRegEnv) */
    uint8_t fxS[kFxScratch], fxR[kFxScratch], fxP[kFxScratch]; /* NrrRegCompile's snapshots */
    /* [native] nrr_hud=2 (verify): the run executes stock; every compiled register word, every
     * rectangle's block (floats, state, TexEnv constants) is checked against what it did */
    struct Vfy {
        uint32_t regOk, regBad, regNew, rectOk, rectBad, rectNew, rectSkip, auxOk, auxBad, loads, ldOk, ldBad,
            progRuns;
    } vfy, vfySes;
    uint32_t vfyLogs;
    uint32_t opaqueLogs;
    uint16_t recMiss[kRecMiss]; /* per run hash: programs that did not fit its entry state */
    /* the program being recorded (NrrDecRun) */
    bool recOk;
    uint8_t recWhy; /* menus-s2 [debug] nimenu_voldiag: why recOk went false (diagnostic) */
    bool decFast; /* the per-word decoder drew a rectangle from a block (NrrProgEx) */
    bool recFull; /* an opaque word: the program depends on every snapshot word */
    uint32_t recN, recEpoch;
    uint32_t recSnap[kSnapMax];
    uint32_t recRead[kSnapMax];  /* bits read before the run wrote them */
    uint32_t recWrote[kSnapMax]; /* bits the run's ST / TL ops wrote */
    uint32_t rec[kRecWords];
    NrrRegFx regs[kRegFx];
    NrrBlock blocks[kBlocks];
    /* [native] nrr_hud_late bit 1 (n60intro): scissor variants of program blocks (NrrScVariant) */
    struct ScVar {
        NrrBlock blk;
        const NrrBlock* base;
        uint32_t baseVer;
        uint32_t seq; /* the task that built it */
    };
    ScVar* scv; /* kScVars, allocated with the lever (nullptr: bit 1 is off) */
    NrrBlock* scSib[kScSibs];   /* a captured block per key-without-scissor (NrrScSibling) */
    void* scState[kScStates];   /* NrrStateBytes each, allocated on first use */
    uint8_t scStateOwner[kScStates]; /* the variant using it + 1, 0 = none */
    uint32_t scStateNext;
    uint32_t blockVer;
    uint32_t slBuf[kSlPayMax]; /* n60hudx: an SL op's payload while it is compiled (NrrSlCompile) */
    /* n60hudx [native] nrr_hud_rr: the capture of a program's execution (its rectangles' output) */
    bool rrCap, rrCapBad, rrCapVfy;
    uint32_t rrCapN;
    uint32_t rrCapEpoch[4]; /* memo epoch, state generation, texture epoch, program epoch at its start */
    uint32_t rrCapSnap[kRrSnap];
    const uint32_t* rrCapOp[kRrMaxRects];
    const NrrBlock* rrCapBlock[kRrMaxRects];
    uint8_t rrCapEmpty[kRrMaxRects];
    float rrCapF[kRrMaxRects][kQuadFloats];
    uint32_t rrCapTask; /* captures this task */
    /* n60hudx: compiled setup lists by G_DL word (NrrDecDl reuses one whose list still matches) */
    struct SlEnt {
        uint32_t w0, w1, w1hi, words; /* words 0 = empty */
        uint32_t pay[kSlCacheWords];
    } slc[kSlCache];
    uint32_t* rrPool; /* kRrPoolWords, nullptr: the lever is off */
    uint32_t rrUsed;
    uint32_t rrBuf[kRrPoolWords / 4]; /* a record's ops while it is built */
    uint32_t pool[kPoolWords];
};

/* The register handlers a word may be compiled from: pure (masked) writes of the registers in
 * NrrRegRegions, whose only other effect is an empty flush's counter (prim / env / prim depth: the
 * hot path runs them only with no interpreter batch open) or the texture arm of a value change
 * (SETTILE / SETTILESIZE). Every other word runs its handler. */
static bool NrrRegCompilable(uint8_t op) {
    switch (op) {
        case OP_SETTIMG:
        case OP_SETTILE:
        case OP_SETTILESIZE:
        case OP_SETCOMBINE:
        case OP_SETOTHERMODE_L:
        case OP_SETOTHERMODE_H:
        case OP_RDPSETOTHERMODE:
#ifndef EXPANSION_KIT
        case OP_SETSCISSOR: /* the EK build's ScissorPillarSafeX reads a runtime lever */
#endif
        case OP_SETPRIMCOLOR:
        case OP_SETENVCOLOR:
        case OP_SETFILLCOLOR:
        case OP_SETPRIMDEPTH:
        case OP_GEOMETRYMODE: return true;
        default: return false;
    }
}

/* A colour / prim-depth / geometry word leaves the last rectangle's key as it was (prim / env
 * reach the corners and the TexEnv constants live; a rectangle draws with geometry mode 0). */
static bool NrrRegDirties(uint8_t op) {
    return op != OP_SETPRIMCOLOR && op != OP_SETENVCOLOR && op != OP_SETFILLCOLOR && op != OP_SETPRIMDEPTH &&
           op != OP_GEOMETRYMODE;
}

/* ---- the hot loop ------------------------------------------------------------------------ *
 *
 * A run is executed from its PROGRAM: compiled once (cold, NrrDecRun: the per-word decoder below
 * runs the run and records what it did) and keyed by the run's skeleton -- every word, except a
 * rectangle's geometry (its coordinates and texture steps: only its opcode and tile count) -- and
 * by the registers the run starts from (the entry snapshot: every register a rectangle's key, a
 * virtual load or a depth-clear test can read, plus the NRR's texture identities). A hit is the
 * skeleton compare, the snapshot compare and the ops:
 *   ST  a register word: its handler's masked stores (NrrRegCompile), the texture arm / empty-flush
 *       counter on a change;
 *   LD  a virtual load: the pending-load record and the identity it names;
 *   RT / FL  a TEXRECT / FILLRECT from its state block (the block its key named at compile: the key
 *       is a function of the skeleton and the entry snapshot);
 *   EX  any other word: the per-word decoder (cold).
 * Anything a program cannot carry on with (an open interpreter batch, a memo clear) hands the rest
 * of the run to the per-word decoder. */

/* mstereo [stereo] menu_depth: the NRR HUD's open batch, for the cold stereo hook (NrrHudStereoAnchor) (which the hot
 * NrrHudHotDraw calls with the context only: a wider call would grow the hot loop). Set once, where the
 * NrrHud is allocated (NrrEnsure; it is never freed). */
static NrrCtx* sStereoHudCtx = nullptr;
static float** sStereoHudBatchStart = nullptr; /* the hook may advance them past runs it drew itself */
static uint32_t* sStereoHudBatchVerts = nullptr;

GDX_COLD bool Interp::NrrEnsure() {
    if (mNrr != nullptr) {
        return true;
    }
    mNrr = new (std::nothrow) NrrHud;
#if defined(__3DS__)
    NRR_CRUMB_FIRST(NRR_CRUMB_HUDOPEN, 4, "hud alloc NrrHud=%p bytes=%lu", (void*)mNrr, (unsigned long)sizeof(NrrHud));
#endif
    if (mNrr == nullptr) {
        return false;
    }
    memset(static_cast<void*>(mNrr), 0, sizeof(NrrHud));
    sStereoHudCtx = &mNrr->ctx; /* mstereo: NrrHudStereoAnchor's view of the open batch */
    sStereoHudBatchStart = &mNrr->batchStart;
    sStereoHudBatchVerts = &mNrr->batchVerts;
    mNrr->clock = mNrrClock;
    mNrr->envSeq = 0xFFFFFFFFu;
    mNrr->batchCls = kNoClass;
    static const unsigned long kNoAtlas = 0; /* == Backend::AtlasEpoch's default */
    const unsigned long* atlas = mBackend->AtlasEpochPtr();
    mNrr->atlasPtr = atlas != nullptr ? atlas : &kNoAtlas;
    NrrSnapInit();
    if ((mNrrLate & kLateScissor) != 0) {
        mNrr->scv = static_cast<NrrHud::ScVar*>(::operator new(sizeof(NrrHud::ScVar) * kScVars, std::nothrow));
        if (mNrr->scv != nullptr) {
            memset(static_cast<void*>(mNrr->scv), 0, sizeof(NrrHud::ScVar) * kScVars);
            GDXN_LOG("[nrrhud] late=%d variants=%lu x %lu B, states %lu x %lu B (on demand)", mNrrLate,
                     static_cast<unsigned long>(kScVars), static_cast<unsigned long>(sizeof(NrrHud::ScVar)),
                     static_cast<unsigned long>(kScStates), static_cast<unsigned long>(NrrStateBytes()));
        } else {
            mNrrLate &= ~kLateScissor; /* no memory: programs keep the scissor as an entry dependency */
        }
    }
    if (mNrrRr != 0) {
        mNrr->rrPool = static_cast<uint32_t*>(::operator new(sizeof(uint32_t) * kRrPoolWords, std::nothrow));
        if (mNrr->rrPool == nullptr) {
            mNrrRr = 0; /* no memory: no replay */
        }
        GDXN_LOG("[nrrhud] rr=%d records %lu B, capture %lu B", mNrrRr,
                 static_cast<unsigned long>(sizeof(uint32_t) * kRrPoolWords), static_cast<unsigned long>(sizeof(NrrHud)));
    }
    if (mNrrHudMemo != 0) {
        mNrr->memo = new (std::nothrow) NrrRectMemo[kRectMemos]; /* nullptr: the memo stays off */
        if (mNrr->memo != nullptr) {
            memset(static_cast<void*>(mNrr->memo), 0, sizeof(NrrRectMemo) * kRectMemos);
        }
    }
    return true;
}

/* A HUD-kind run, decoded. Returns false when the NRR cannot take it (the replay runs). */
bool Interp::NrrHudHotRun(const void* runWords, uint32_t n, uint32_t mode) {
    (void)mode;
    if ((mNrr == nullptr && !NrrEnsure()) || gdx3ds_replay_interp != nullptr || mVerifyActive || n == 0) {
        return false;
    }
    const GdxHudWord* w = static_cast<const GdxHudWord*>(runWords);
    if (GDX_UNLIKELY(mNrr->clock != 0 || mNrrHud == 2)) {
        NrrRunCold(w, n);
    } else {
        NrrHudHotBody(w, n);
    }
    return true;
}

inline __attribute__((always_inline)) void Interp::NrrHudHotBody(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    N.w.runs++;
    N.w.words += n;
    N.runWords = w;
    N.runN = n;
    N.drew = false;
    if (GDX_UNLIKELY(N.atlasEpoch != *N.atlasPtr || N.envSeq != N.seq ||
                     N.envEgm != mRsp.extra_geometry_mode)) {
        NrrRunEpochs();
    }
    if (GDX_UNLIKELY(mBufNumTris != 0) && (mNrrLate & kLateFlush) != 0 && mTexOv == nullptr) {
        NrrLateFlush();
    }
    const NrrProg* p = (mBufNumTris == 0 && mTexOv == nullptr) ? NrrHudHotProg(w, n) : nullptr;
    if (p != nullptr) {
        const uint32_t* op = N.pool + p->ops;
        uint32_t pos = 0;
        if (GDX_UNLIKELY(mNrrRr != 0)) {
            op = NrrHudHotRrTry(*p, w, n, &pos); /* n60hudx: nullptr = replayed (or finished) */
        }
        if (op != nullptr) {
            NrrHudHotExec(*p, w, n, op, pos);
            if (GDX_UNLIKELY(N.rrCap)) {
                NrrRrCapEnd(*p, w, n);
            }
        }
    } else {
        NrrDecRun(w, n);
    }
    if (N.drew) {
        mRdp.viewport_or_scissor_changed = true; /* == DrawRectangle's exit: the 3D viewport is re-checked */
    }
}

/* o3ds-cpucuts [native] nrr_hud_pd (0 off, 1 on, 2 A/B on [cpucut] bit 0; default 1): a SETPRIMDEPTH
 * is a COL op -- any depth, out of the skeleton hash -- instead of a compiled word. The race's
 * opponent markers set a new prim depth every frame, so each frame was a new skeleton (no program:
 * decoded word by word, its depth word a first sight that refused the recording, the stores that did
 * land filled the pool and cleared every program). The depth's handler effect (the 15-bit z into
 * prim_depth, the empty flush counted on a change) is fixed by its code, as a colour's is; latched
 * per task (NrrHudTaskEnd), so a run's hash and its recording agree. */
static int sNrrHudPd = 1;
static bool sNrrHudPdOn = true;

extern "C" void gdx3ds_nrr_hud_pd_set(int value) {
    sNrrHudPd = gdx3ds_cpucut_clamp(value, 1);
    sNrrHudPdOn = gdx3ds_cpucut_on(sNrrHudPd, GDX3DS_CPUCUT_BIT_HUDPD) != 0;
    gdx3ds_cpucut_lever("hudpd", &sNrrHudPd, GDX3DS_CPUCUT_BIT_HUDPD);
}

/* The run's skeleton hash: every word but a rectangle's geometry (a TEXRECT with its trailers is
 * its opcode and tile, a FILLRECT its opcode) and a colour word's colour. Only a lookup key: every
 * op checks its own word. */
__attribute__((noinline)) uint32_t NrrHudHotHash(const GdxHudWord* w, uint32_t n) {
    uint32_t h = n;
    NRR_NO_UNROLL
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t op = w[i].w0 >> 24;
        uint32_t a = w[i].w0;
        uint32_t b = w[i].w1;
        if ((op == OP_TEXRECT || op == OP_TEXRECTFLIP) && i + 2 < n) {
            a &= 0xFF000000u;
            b &= 7u << 24;
            i += 2;
        } else if (op == OP_FILLRECT) {
            a &= 0xFF000000u;
            b = 0;
        } else if (op == OP_SETPRIMCOLOR || op == OP_SETENVCOLOR) {
            b = 0; /* a COL op takes any colour */
        } else if (op == OP_SETPRIMDEPTH && sNrrHudPdOn) {
            b = 0; /* nrr_hud_pd: a COL op takes any prim depth */
        }
        h = ((h << 5) | (h >> 27)) ^ a;
        h = ((h << 5) | (h >> 27)) ^ b;
    }
    return h * 0x9E3779B1u; /* every bit into the top ones, which pick the slot (NrrProgSlot) */
}

/* A skeleton hash's first program slot: its TOP bits (NrrHudHotHash ends in a multiply, so every
 * bit of the rotate-xor feeds them). Its low bits see few bits of each word: font-glyph runs, which
 * differ only in their SETTIMG address, crowded ~16 probe chains and evicted each other every frame. */
inline uint32_t NrrProgSlot(uint32_t h) {
    return h >> (32u - kProgBits);
}

/* The run's program: its hash's, whose entry dependencies the live registers match. */
inline __attribute__((always_inline)) const Interp::NrrProg* Interp::NrrHudHotProg(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    const uint32_t h = NrrHudHotHash(w, n);
    const uint32_t slot0 = NrrProgSlot(h);
    NRR_NO_UNROLL
    for (uint32_t p = 0; p < kProgProbe; p++) {
        const NrrProg& g = N.progs[(slot0 + p) & (kProgs - 1u)];
        if (g.hash != h || g.n != n) {
            continue;
        }
        const uint32_t* d = N.pool + g.deps;
        uint32_t k = 0;
        while (k < g.nDeps && ((*reinterpret_cast<const NrrWord32*>(d[0]) ^ d[2]) & d[1]) == 0) {
            k++;
            d += 3;
        }
        if (k == g.nDeps) {
            return &g;
        }
    }
    return nullptr;
}

/* Which bits of its word (w0, w1, w1hi) an op checks against the word it was compiled from: an ST
 * all of them, an LD its LOADBLOCK, an RT the opcode and tile, an FL the opcode, an EX none (the
 * per-word decoder re-decides it). */
const uint32_t kOpCheck[8][3] = { { 0, 0, 0 },
                                  { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu },
                                  { 0xFFFFFFFFu, 0xFFFFFFFFu, 0 },
                                  { 0xFF000000u, 7u << 24, 0 },
                                  { 0xFF000000u, 0, 0 },
                                  { 0, 0, 0 },
                                  { 0xFF000000u, 0, 0xFFFFFFFFu },
                                  { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu } };

/* A program's ops (see the section comment). Every op checks its word (kOpCheck) and that the ops
 * cover the run word by word; an RT op also checks the texture at its tile's TMEM address. A
 * mismatch hands the run, from that word, to the per-word decoder (NrrProgMiss). */
inline __attribute__((always_inline)) void Interp::NrrHudHotExec(const NrrProg& p, const GdxHudWord* w, uint32_t n,
                                                                const uint32_t* op, uint32_t pos) {
    NrrHud& N = *mNrr;
    const uint32_t* const end = N.pool + p.ops + p.opWords;
    const uint32_t epoch = N.progEpoch;
    while (op < end) {
        const uint32_t hdr = op[0];
        const uint32_t kind = hdr & 7u;
        const uint32_t i = hdr >> 16;
        const Cmd c = { w[i].w0, w[i].w1, w[i].w1hi };
        const uint32_t* m = kOpCheck[kind];
        if (GDX_UNLIKELY((((c.w0 ^ op[1]) & m[0]) | ((c.w1 ^ op[2]) & m[1]) | ((c.w1hi ^ op[3]) & m[2]) | (i ^ pos)) != 0 ||
                         (kind == kOpRt && !NrrHudHotIdent(op + 5, mRdp.texture_tile[C1(c.w1, 24, 3)].tmem_index)) ||
                         (kind == kOpLd && (mNrrLate & kLatePitch) != 0 && mRdp.texture_to_load.width > 1 &&
                          !NrrLdPitchOk(op + 4, c)) ||
                         (kind == kOpFl && mRdp.color_image_address == mRdp.z_buf_address))) {
            NrrProgMiss(p, w, n, pos); /* another run under this hash, or another texture at the tile's TMEM */
            return;
        }
        op += 4;
        pos = i + 1;
        if (kind == kOpSt) {
            op = NrrHudHotStores(hdr, op);
        } else if (kind == kOpLd) {
            NrrHudHotLoad(op, c);
            op += kLdWords;
        } else if (kind == kOpEx) {
            pos = NrrProgEx(p, w, n, i);
        } else if (kind == kOpCol) {
            NrrHudHotColour(hdr, op, c);
            op += 3;
        } else if (kind == kOpSl) {
            if (!NrrHudHotSl(op, c, p, w, n, i)) {
                return; /* NrrSlMiss finished the run */
            }
            op += op[3] >> 16;
        } else {
            NrrHudHotRect(reinterpret_cast<NrrBlock*>(op[0]), c, kind == kOpRt ? w + i : nullptr, op);
            pos = kind == kOpRt ? i + 3 : pos;
            op += kind == kOpRt ? kRtWords : 1u;
        }
        if (GDX_UNLIKELY(N.progEpoch != epoch || mBufNumTris != 0 || mTexOv != nullptr)) {
            NrrDecFrom(w, n, pos); /* the program cannot go on: the per-word decoder finishes the run */
            return;
        }
    }
    if (GDX_UNLIKELY(pos != n)) {
        NrrProgMiss(p, w, n, pos);
    }
}

/* An ST op: the word's masked stores into the live registers (NrrRegCompile); on a change, the
 * handler's conditional effects -- the texture arm, the empty flush's counter. Returns the next op. */
inline __attribute__((always_inline)) const uint32_t* Interp::NrrHudHotStores(uint32_t hdr, const uint32_t* st) {
    const uint32_t n = (hdr >> 4) & 15u;
    uint32_t changed = 0;
    NRR_NO_UNROLL
    for (uint32_t k = 0; k < n; k++, st += 3) {
        NrrWord32* d = reinterpret_cast<NrrWord32*>(st[0]);
        const uint32_t o = *d;
        const uint32_t v = (o & ~st[1]) | st[2];
        changed |= o ^ v;
        *d = v;
    }
    if (changed != 0) {
        if ((hdr & kOpArm) != 0) {
            mRdp.textures_changed[0] = true; /* == ArmTexturesChanged */
            mRdp.textures_changed[1] = true;
        }
        const uint32_t why = (hdr >> 9) & 31u;
        if (why != FLUSH_NONE) {
            mStats.flush[why]++; /* == the handler's flush of an empty batch */
        }
    }
    return st;
}

/* A COL op (SETPRIMCOLOR / SETENVCOLOR, whatever its colour): the register is the word's w1 in
 * memory order (RgbaOf), prim's lod fraction its w0's low byte -- as the handler's compiled
 * stores were found to be (NrrRecRegWord); a change counts the handler's empty flush. */
inline __attribute__((always_inline)) void Interp::NrrHudHotColour(uint32_t hdr, const uint32_t* op, const Cmd& c) {
    NrrWord32* col = reinterpret_cast<NrrWord32*>(op[0]);
    if (GDX_UNLIKELY((hdr & kOpColPd) != 0)) {
        const uint32_t o = *col; /* == DpSetPrimDepth((w1 >> 16) & 0x7FFF) */
        const uint32_t nv = (o & ~(0xFFFFu << op[2])) | (((c.w1 >> 16) & 0x7FFFu) << op[2]);
        *col = nv;
        if (o != nv) {
            mStats.flush[(hdr >> 9) & 31u]++;
        }
        return;
    }
    const uint32_t v = __builtin_bswap32(c.w1);
    uint32_t changed = *col ^ v;
    *col = v;
    if (op[1] != 0) {
        NrrWord32* lod = reinterpret_cast<NrrWord32*>(op[1]);
        const uint32_t o = *lod;
        const uint32_t nv = (o & ~(0xFFu << op[2])) | ((c.w0 & 0xFFu) << op[2]);
        changed |= o ^ nv;
        *lod = nv;
    }
    if (changed != 0) {
        mStats.flush[(hdr >> 9) & 31u]++;
    }
}

/* n60hudx [native] nrr_hud_kept: an SL op -- a G_DL the capture recorded inside the run, to one of the
 * HUD's static setup lists (pure register words). == Exec's G_DL walking the list: the resolve and the
 * G_DL hook run as Exec runs them (the dlmemo or the bridge: the same answer, the same memo side
 * effects); the resolved list must be the one compiled -- the same base / bound / stride / class and
 * every word through its terminator -- and then its words' stores apply (each word's handler as
 * masked stores, NrrSlCompile). Before the list, what the end of the run before it did when the G_DL
 * was a kept word between two runs (NrrHudHotBody's exit). Payload: base, limit, stride | big << 8 |
 * f3d << 9, nCmd | payload words << 16, 2 x nCmd raw list words, then ST groups. false: NrrSlMiss
 * finished the run. */
__attribute__((noinline)) bool Interp::NrrHudHotSl(const uint32_t* pay, const Cmd& c, const NrrProg& p,
                                                   const GdxHudWord* w, uint32_t n, uint32_t i) {
    NrrHud& N = *mNrr;
    if (N.drew) {
        mRdp.viewport_or_scissor_changed = true; /* == the run before the G_DL ending */
        N.drew = false;
    }
    ListRef sub;
    int why = 0;
    if (!((mDlMemoLevel != 0) ? ResolveListMemo(c, sub, false) : mSrc.ResolveList(c, sub))) {
        mStats.noopLists++;
        why = 1;
    } else if (mDlHook != nullptr && (mDlHookSub = sub, mDlHook(mDlHookUser, sub.base, this) != 0)) {
        why = 2;
    } else {
        const uint32_t ref = static_cast<uint32_t>(sub.stride) | (sub.isBig ? 0x100u : 0u) | (sub.isF3d ? 0x200u : 0u);
        why = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sub.base)) == pay[0] &&
               static_cast<uint32_t>(sub.limit) == pay[1] && ref == pay[2] &&
               memcmp(sub.base, pay + 4, (pay[3] & 0xFFFFu) * sub.stride) == 0)
                  ? 0
                  : 3;
    }
    if (GDX_UNLIKELY(why != 0)) {
        NrrSlMiss(p, w, n, i, sub, why);
        return false;
    }
    NrrHudHotGroups(pay + 4 + NrrSlRawWords(pay), pay + (pay[3] >> 16));
    N.w.slHits++;
    return true;
}

/* ST groups [g, end) (an SL op's list, a replay's NS block): each group's masked stores, its
 * conditional effects on a change (NrrHudHotStores). */
__attribute__((noinline)) void Interp::NrrHudHotGroups(const uint32_t* g, const uint32_t* end) {
    while (g < end) {
        g = NrrHudHotStores(g[0], g + 1);
    }
}


/* ---- n60hudx [native] nrr_hud_rr: run replay ------------------------------------------------- *
 *
 * A run whose program executed without a miss is RECORDED (NrrRrCapEnd, cold): its words, the entry
 * words its rectangles' vertices read besides them (NrrHudHotRrSnap: the prim / env / fill colours, the
 * four corner colours, the extra geometry mode, the scissor), the epochs every block's freshness
 * rests on (the memo epoch: a block capture, a program clear, a corner-mapping change; the core's
 * state generation; the texture cache's retire epoch), and a replay op stream derived from the
 * program: each maximal sequence of register ops (ST / COL) as merged masked stores (NS), the loads
 * (LD) and setup lists (SL) as the program's own ops, and every rectangle as its block and the six
 * vertices it produced.
 *
 * Next time the program is found (its entry dependencies matched), the run REPLAYS when its words
 * are byte-identical to the record's and the epochs and entry words are the recorded ones: the NS
 * stores, the LD / SL ops (the program's own code: the pending loads, the identities, the list
 * resolve and hook), and per rectangle the program's own checks (its block, the texture identity
 * at its tile, the depth-clear test), the reservation exactly as the program's rectangle makes it
 * (NrrHudHotReserve: the simulated viewport / scissor, the batch split, the state block, the TexEnv
 * constants from the live registers -- which are the program's at that point) and the recorded
 * vertices. A rectangle's vertices are a function of its block, its words, the colour sources it
 * reads, the cycle type and the corner mapping (the nrr_hud_memo key, n60gp): all fixed by the
 * checks. What a replay does not repeat: the rect memo's lookups (a cache) and the empty-flush
 * counters of register words (statistics). A check that fails mid-run hands the rest of the run to
 * the program from that op (every op before it applied exactly as the program would have).
 * ------------------------------------------------------------------------------------------------ */

/* The entry words a record is checked on (besides its words, its epochs and its program's deps). */
void Interp::NrrHudHotRrSnap(uint32_t* out) const {
    memcpy(&out[0], &mRdp.prim_color, 4);
    memcpy(&out[1], &mRdp.env_color, 4);
    memcpy(&out[2], &mRdp.fill_color, 4);
    const LoadedVertex* lv = &mRsp.loaded_vertices[kMaxVertices];
    NRR_NO_UNROLL
    for (int k = 0; k < 4; k++) {
        memcpy(&out[3 + k], &lv[k].color, 4);
    }
    out[7] = mRsp.extra_geometry_mode;
    memcpy(&out[8], &mRdp.scissor, sizeof(Rect));
    out[11] = mRdp.prim_lod_fraction;
}

/* A replayed rectangle (q: its replay op {hdr, the program's op, the block}; f its six vertices): the
 * program's own checks (its block, the texture identity at its tile / the depth-clear test), the fill
 * colour into the corners (a FILLRECT), the reservation the program's rectangle makes, the vertices.
 * false: a check failed (nothing done: the program takes the op). */
__attribute__((noinline)) bool Interp::NrrHudHotRectFrom(const uint32_t* q, const GdxHudWord* w, const float* f) {
    const uint32_t hdr = q[0];
    const uint32_t i = hdr >> 16;
    const uint32_t* pp = reinterpret_cast<const uint32_t*>(q[1]) + 4;
    const bool fill = (hdr & 7u) == kRrFl;
    NrrBlock& b = *reinterpret_cast<NrrBlock*>(q[2]);
    if (pp[0] != q[2] || (fill ? mRdp.color_image_address == mRdp.z_buf_address
                               : !NrrHudHotIdent(pp + 1, mRdp.texture_tile[(hdr >> 8) & 7u].tmem_index))) {
        return false;
    }
    /* == the program rectangle's freshness test; the record's epochs hold every part of it but the
     * per-task check of a texture source that is not immutable (NrrBlockFresh's recheck) */
    if (b.usesTex != 0 && b.imm == 0 && b.checkedSeq != mNrr->seq && !NrrBlockRecheck(b)) {
        return false;
    }
    mNrr->w.rrRects++;
    if (fill && (hdr & (1u << 11)) != 0) {
        mStats.rects++; /* an empty FILLRECT: counted, nothing drawn */
        return true;
    }
    if (fill) {
        NRR_NO_UNROLL
        for (int k = kMaxVertices; k < kMaxVertices + 4; k++) {
            mRsp.loaded_vertices[k].color = mRdp.fill_color;
        }
    }
    float* p = NrrHudHotReserve(b);
    const Cmd c = { w[i].w0, w[i].w1, w[i].w1hi };
    if (p == nullptr) {
        NrrNoRing(c, fill ? nullptr : w + i, fill);
        return true;
    }
    memcpy(p, f, kQuadFloats * sizeof(float));
    return true;
}

/* The program found for a run: replay its record when it fits (returns nullptr: the run is done),
 * else the op to execute the program from (*pos its word). Arms a capture when the program has no
 * record and may get one. */
__attribute__((noinline)) const uint32_t* Interp::NrrHudHotRrTry(const NrrProg& p, const GdxHudWord* w, uint32_t n,
                                                                 uint32_t* pos) {
    NrrHud& N = *mNrr;
    *pos = 0;
    if (p.rr == 0) {
        NrrRrArm(p);
        return N.pool + p.ops;
    }
    const uint32_t* r = N.rrPool + (p.rr - 1u);
    uint32_t snap[kRrSnap];
    NrrHudHotRrSnap(snap);
    if (r[0] != n || r[1] != N.memoEpoch || r[2] != NrrStateGeneration() || r[3] != mNrrTexEpoch ||
        memcmp(snap, r + 6, sizeof(snap)) != 0 || memcmp(w, r + kRrHead, n * sizeof(GdxHudWord)) != 0 ||
        (mNrrRr & 4) != 0) {
        NrrRrGateOut(p); /* recorded again, or verified */
        return N.pool + p.ops;
    }
    const uint32_t* q = r + kRrHead + 4u * n;
    const uint32_t* const qe = q + r[4];
    const float* f = reinterpret_cast<const float*>(qe);
    const uint32_t epoch = N.progEpoch;
    while (q < qe) {
        const uint32_t hdr = q[0];
        const uint32_t kind = hdr & 7u;
        if (kind == kRrNs) {
            NrrHudHotGroups(q + 1, q + 1 + (hdr >> 16));
            q += 1 + (hdr >> 16);
            continue;
        }
        const uint32_t i = hdr >> 16;
        const uint32_t* po = reinterpret_cast<const uint32_t*>(q[1]);
        const Cmd c = { w[i].w0, w[i].w1, w[i].w1hi };
        uint32_t next = i + 1;
        if (kind == kRrLd) {
            if ((mNrrLate & kLatePitch) != 0 && mRdp.texture_to_load.width > 1 && !NrrLdPitchOk(po + 4, c)) {
                break;
            }
            NrrHudHotLoad(po + 4, c);
            q += 2;
        } else if (kind == kRrSl) {
            if (!NrrHudHotSl(po + 4, c, p, w, n, i)) {
                return nullptr; /* NrrSlMiss finished the run */
            }
            q += 2;
        } else {
            if (!NrrHudHotRectFrom(q, w, f)) {
                break;
            }
            f += (kind == kRrFl && (hdr & (1u << 11)) != 0) ? 0u : kQuadFloats;
            next = kind == kRrFl ? i + 1 : i + 3;
            q += 3;
        }
        if (GDX_UNLIKELY(N.progEpoch != epoch || mBufNumTris != 0 || mTexOv != nullptr)) {
            NrrDecFrom(w, n, next);
            return nullptr;
        }
    }
    if (q < qe) {
        N.w.rrResume++; /* a check failed at this op: the program from here (every op before it applied) */
        *pos = q[0] >> 16;
        return reinterpret_cast<const uint32_t*>(q[1]);
    }
    N.w.rrHits++;
    const_cast<NrrProg&>(p).rrInfo &= (1u << kRrMissShift) - 1u; /* a replay: its misses start over */
    return nullptr;
}

/* ---- loads ------------------------------------------------------------------------------- */

/* A virtual load (an LD op, or the decoder's): r = {SETTIMG w0, w1, LOADBLOCK w0, w1, lo | hi << 16}.
 * Appended to the pending loads (one it covers whole is dead: emptied in place, so the live ones
 * keep their order) and named as the identity of TMEM [lo, hi). With the pending list full it
 * loads for real (NrrLoadCold). */
void Interp::NrrHudHotLoad(const uint32_t* r, const Cmd& c) {
    NrrHud& N = *mNrr;
    if (GDX_UNLIKELY(N.nPend >= kPend)) {
        NrrPendMakeRoom();
    }
    const uint32_t lo = r[4] & 0xFFFFu;
    const uint32_t hi = r[4] >> 16;
    NRR_NO_UNROLL
    for (uint32_t k = 0; k < N.nPend; k++) {
        NrrHud::Pend& d = N.pend[k];
        if (d.lo >= lo && d.hi <= hi) {
            d.hi = d.lo; /* dead: covers nothing */
        }
    }
    NrrHud::Pend& p = N.pend[N.nPend++];
    p.timgW0 = r[0];
    p.timgW1 = r[1];
    p.lw0 = r[2];
    p.lw1 = r[3];
    p.lo = static_cast<uint16_t>(lo);
    p.hi = static_cast<uint16_t>(hi);
    NrrHudHotSlot(lo, hi, r);
    mRdp.textures_changed[0] = true; /* == DpLoadBlock's exit (ArmTexturesChanged) */
    mRdp.textures_changed[1] = true;
    mStats.tmemLoads++;
    mNrrTmemLive = true;
}

/* What sits at TMEM [lo, hi) is now the load r's source (SETTIMG w0 / w1, LOADBLOCK w0 / w1 without
 * its tile index): identities it overlaps are gone. */
void Interp::NrrHudHotSlot(uint32_t lo, uint32_t hi, const uint32_t* r) {
    NrrHud& N = *mNrr;
    NrrHud::Slot* freeSlot = nullptr;
    NRR_NO_UNROLL
    for (uint32_t k = 0; k < kSlots; k++) {
        NrrHud::Slot& s = N.slots[k];
        if (s.lo < hi && s.hi > lo) {
            s.hi = s.lo; /* gone (an empty range is a free slot) */
        }
        if (s.hi == s.lo && freeSlot == nullptr) {
            freeSlot = &s;
        }
    }
    if (freeSlot != nullptr) {
        freeSlot->lo = static_cast<uint16_t>(lo);
        freeSlot->hi = static_cast<uint16_t>(hi);
        freeSlot->src[0] = r[0];
        freeSlot->src[1] = r[1];
        freeSlot->src[2] = r[2];
        freeSlot->src[3] = r[3] & ~(7u << 24); /* the load tile index is not part of the texture */
    }
}

/* Does the live identity at TMEM word `lo` (the slot starting there) name `src`? */
inline __attribute__((always_inline)) bool Interp::NrrHudHotIdent(const uint32_t* src, uint32_t lo) const {
    const NrrHud& N = *mNrr;
    NRR_NO_UNROLL
    for (uint32_t k = 0; k < kSlots; k++) {
        const NrrHud::Slot& s = N.slots[k];
        if (s.hi != s.lo && s.lo == lo) {
            return s.src[0] == src[0] && s.src[1] == src[1] && s.src[2] == src[2] && s.src[3] == src[3];
        }
    }
    return false;
}

/* ---- rectangles -------------------------------------------------------------------------- */

/* n60stereo, stereo frame replay only: a batch of prim-depth rectangles (the race position markers)
 * takes the [anchor] class -- the depth of the machine it labels -- as the per-draw loop gives it.
 * mstereo: in a menu frame ([stereo] menu_depth) any other batch takes its coverage layer's row, the
 * per-draw loop's MenuUiClass. */
GDX_COLD static void NrrHudStereoAnchor(NrrCtx* ctx, const Combiner* comb, uint16_t primDepth) {
    if (comb != nullptr && (comb->shader_id1 & SHOPT_PRIM_DEPTH) != 0) {
        NrrStereoAnchor(ctx, static_cast<float>(primDepth) / 32767.0f);
    } else if (gdx3ds_stereo_menu_frame() && ctx == sStereoHudCtx) {
        NrrStereoMenuRow(ctx, sStereoHudBatchStart, sStereoHudBatchVerts);
    }
}

/* The open batch, drawn through the core's 2D draw. */
void Interp::NrrHudHotDraw() {
    NrrHud& N = *mNrr;
    if (N.batchVerts != 0) {
        if (GDX_UNLIKELY(gdx3ds_stereo_replay_frame)) {
            NrrHudStereoAnchor(&N.ctx, N.batchBlock != nullptr ? N.batchBlock->ds.comb : nullptr, mRdp.prim_depth);
        }
        NrrDraw2D(&N.ctx, N.batchStart, N.batchVerts);
        N.batchVerts = 0;
        N.w.draws++;
    }
}

/* Does `b` (class `cls`) need a batch of its own? A new class, or a register the open batch's
 * TexEnv constants were read from moved. A block other than the batch's first is a texture-cache
 * hit of its own (the LRU refresh, == TexCacheLookup; a no-op on the LRU tail). */
__attribute__((noinline)) bool Interp::NrrHudHotSplits(const NrrBlock& b, uint32_t cls) {
    NrrHud& N = *mNrr;
    if (N.batchVerts == 0 || cls != N.batchCls) {
        return true;
    }
    if (&b != N.batchBlock && b.usesTex && mTexLruTail != b.slot) {
        TexCacheTouch(b.slot);
    }
    const uint32_t moved = ((N.batchPrim ^ NrrRgbaWord(mRdp.prim_color)) != 0 ? NRR_CONST_PRIM : 0u) |
                           ((N.batchEnv ^ NrrRgbaWord(mRdp.env_color)) != 0 ? NRR_CONST_ENV : 0u) |
                           ((N.batchShade ^ NrrRgbaWord(mRsp.loaded_vertices[kMaxVertices].color)) != 0
                                ? NRR_CONST_SHADE
                                : 0u);
    return (moved & N.batchConst) != 0;
}

/* == HudPackAux (no fog: a fog block is a stock key) through the block's byte selectors: every
 * input channel is its register byte / 255.0f, as GdxInput's colour over 255. */
void Interp::NrrHudHotAux(const NrrBlock& b, uint32_t prim, uint32_t env, uint32_t shade, float out[7][4]) const {
    const uint32_t reg[4] = { prim, env, shade, mRdp.prim_lod_fraction };
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(reg);
    const uint8_t* sel = &b.auxSel[0][0];
    float* o = &out[0][0];
    NRR_NO_UNROLL
    for (uint32_t k = 0; k < 4u * b.auxN; k++) {
        o[k] = sel[k] == 0xFFu ? 1.0f : static_cast<float>(bytes[sel[k]]) / 255.0f;
    }
}

/* A new batch under `b`: the state block applied, its TexEnv constants (SpTri's vertex-0 record,
 * HudPackAux: cached in the block while the registers they read hold), the registers they came
 * from remembered. */
void Interp::NrrHudHotBatch(NrrBlock& b, bool noSc, uint32_t cls) {
    NrrHud& N = *mNrr;
    const NrrState* st = noSc ? b.stNoSc : b.st;
    if (st != N.batchSt) {
        NrrStateApply(&N.ctx, st);
        N.batchSt = st;
    }
    N.batchBlock = &b;
    N.batchCls = cls;
    if (b.usesTex && mTexLruTail != b.slot) {
        TexCacheTouch(b.slot);
    }
    const uint32_t prim = NrrRgbaWord(mRdp.prim_color);
    const uint32_t env = NrrRgbaWord(mRdp.env_color);
    const uint32_t shade = NrrRgbaWord(mRsp.loaded_vertices[kMaxVertices].color);
    float aux[7][4];
    if (GDX_UNLIKELY(b.auxExotic != 0)) {
        NrrAuxCold(b, aux);
    } else {
        NrrHudHotAux(b, prim, env, shade, aux);
    }
    NrrSetInputs(&N.ctx, st, aux, b.auxN);
    N.batchPrim = prim;
    N.batchEnv = env;
    N.batchShade = shade;
    N.batchConst = b.constRegs;
}

/* Room for one rectangle's two triangles in the ring, inside the open batch or a new one.
 * nullptr = dropped (the core refused the hand-off, or the frame's pool is out). First, the stock
 * path's viewport / scissor calls for this rectangle on the simulated backend state: a rectangle
 * sets the default viewport (SetViewport turns citro3d's scissor off) and then its scissor only
 * when it differs from the last one set; `noSc` = the stock rectangle draws with the scissor off. */
__attribute__((noinline)) float* Interp::NrrHudHotReserve(NrrBlock& b) {
    NrrHud& N = *mNrr;
    if (!N.open && !NrrHudOpen()) {
        return nullptr;
    }
    if (!N.simVpDefault) {
        NrrSimDefaultViewport();
    }
    if (!NrrRectEq(N.simSc, b.scissor)) {
        N.simSc = b.scissor;
        N.simLive = true;
        N.simScSet = true;
        N.simLastVp = false;
    }
    const bool noSc = !N.simLive;
    if (noSc && b.stNoSc == nullptr && !NrrResolveNoScissor(b)) {
        return nullptr;
    }
    const uint32_t cls = noSc ? b.batchClsNoSc : b.batchCls;
    const bool split = NrrHudHotSplits(b, cls);
    float* p = static_cast<float*>(NrrRingAlloc(&N.ctx, kQuadFloats * sizeof(float)));
    if (p == nullptr) {
        return nullptr;
    }
    if (split || p != N.batchEnd) {
        NrrHudHotDraw();
        NrrHudHotBatch(b, noSc, cls);
        N.batchStart = p;
    }
    N.batchEnd = p + kQuadFloats;
    N.batchVerts += 6;
    N.drew = true;
    mStats.rects++;
    return p;
}

/* The core hands citro3d back (before an interpreter draw, a non-HUD marker, the task end). */
void Interp::NrrHudHotClose() {
    if (mNrr->open) {
        NrrHudClose();
    }
}

/* One texture coordinate of HudPackVerts (unit 0): 10.5 -> the shift, the tile origin, the
 * normalisation, the pow2 / atlas scale, the clamp, the atlas view offset. axis 0 = u, 1 = v. */
__attribute__((noinline)) float NrrHudHotTexCoord(const Interp::NrrBlockUv& q, int axis, float s) {
    float u = s / 32.0f;
    u *= q.pre[axis];
    u -= q.off[axis];
    const float n = q.mul[axis] ? u * q.normMul[axis] : u / q.normDiv[axis];
    const float p = NrrFmin(n * q.scale[axis], q.clamp[axis]);
    return q.hasOff ? p + q.viewOff[axis] : p;
}

/* == HudPackVerts for the four corners of a rectangle (unit 0 only; a block that samples unit 1
 * is a stock key), written straight into its six ring slots (SpTri's two triangles, ul ll ur and
 * ll lr ur): e = {ulx, uly, lrx, lry} in 10.2 screen units, st = {s0, s1, t0, t1} in 10.5 (s0/t0
 * the upper-left corner's), flip = TEXRECTFLIP. x: NDC, then AdjXForAspectRatio as a multiply and
 * a divide that are exact identities when it does not apply (NrrXMode: 1.0f); y: NDC, negated
 * under an inverted clip (an exact sign). NrrCornersCold: a widescreen-UI x, a register colour. */
inline __attribute__((always_inline)) void Interp::NrrHudHotCorners(const NrrBlock& b, const int32_t e[4], const float st[4], bool flip, float* out) const {
    if (GDX_UNLIKELY(b.coldColour != 0 || mNrr->xFull)) {
        NrrCornersCold(b, e, st, flip, out);
        return;
    }
    NrrHudHotCornersAt(b, e, st, flip, b.colSrc, b.alphaSrc, out);
}

/* The plain corner mapping with the colour / alpha sources given (NrrCornersCold passes its own
 * instead of copying the block). */
inline __attribute__((always_inline)) void Interp::NrrHudHotCornersAt(const NrrBlock& b, const int32_t e[4], const float st[4], bool flip,
                                                                     const Rgba* colSrc, const Rgba* alphaSrc, float* out) const {
    const NrrHud& N = *mNrr;
    /* e = {ulx, uly, lrx, lry} -> {x0, y0, x1, y1}: x = (e / w - 1) * xMul / xDiv (the
     * AdjXForAspectRatio pair, or 1 / 1), y = (1 - e / h) * ySign / 1 (== -(e / h) + 1, negated
     * under an inverted clip; 1 - v, not -(v - 1): the sign of a zero must match) */
    float xy[4], tc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    NRR_NO_UNROLL
    for (int k = 0; k < 4; k++) {
        const int a = k & 1;
        const float v = static_cast<float>(e[k]) / N.xyHalf[a];
        xy[k] = (a != 0 ? 1.0f - v : v - 1.0f) * (a != 0 ? b.ySign : N.xMul) / N.xyDiv[a];
    }
    if (b.usesTex) {
        NRR_NO_UNROLL
        for (int k = 0; k < 4; k++) {
            tc[k] = NrrHudHotTexCoord(b.uv, k >> 1, st[k]);
        }
    }
    /* the six slots (SpTri's ul ll ur, ll lr ur) as corners ul 0, ll 1, lr 2, ur 3: x index i >> 1,
     * y index ((i + 1) >> 1) & 1; the flip swaps which of them u and v follow */
    static const uint8_t kSlotCorner[6] = { 0, 1, 3, 1, 2, 3 };
    const LoadedVertex* lv = &mRsp.loaded_vertices[kMaxVertices];
    const Rgba* cBase = colSrc != nullptr ? colSrc : &lv[0].color;
    const Rgba* aBase = alphaSrc != nullptr ? alphaSrc : &lv[0].color;
    const uint32_t cStep = colSrc != nullptr ? 0u : static_cast<uint32_t>(sizeof(LoadedVertex));
    const uint32_t aStep = alphaSrc != nullptr ? 0u : static_cast<uint32_t>(sizeof(LoadedVertex));
    const float* const unit = mByteToUnit;
    NRR_NO_UNROLL
    for (int s = 0; s < 6; s++) {
        const uint32_t i = kSlotCorner[s];
        const uint32_t xi = i >> 1;
        const uint32_t yi = ((i + 1) >> 1) & 1u;
        const Rgba& cc = *reinterpret_cast<const Rgba*>(reinterpret_cast<const uint8_t*>(cBase) + i * cStep);
        const Rgba& ac = *reinterpret_cast<const Rgba*>(reinterpret_cast<const uint8_t*>(aBase) + i * aStep);
        float* o = out + s * kStride;
        o[0] = xy[xi * 2];
        o[1] = xy[yi * 2 + 1];
        o[2] = b.z;
        o[3] = 1.0f;
        o[4] = tc[flip ? yi : xi];
        o[5] = tc[2 + (flip ? xi : yi)];
        o[6] = unit[cc.r];
        o[7] = unit[cc.g];
        o[8] = unit[cc.b];
        o[9] = unit[ac.a];
        o[10] = 0.0f;
        o[11] = 0.0f;
    }
}

/* A rectangle's geometry, == DpTextureRectangle / DpFillRectangle: e = {ulx, uly, lrx, lry} (10.2;
 * COPY / FILL widen by a pixel, a full-screen fill widens to the panel), st = {s0, s1, t0, t1}
 * (10.5; the COPY cycle's dsdx step, the flip). false = an empty FILLRECT (nothing drawn). */
inline __attribute__((always_inline)) bool Interp::NrrHudHotGeom(const NrrBlock& b, const Cmd& c, const GdxHudWord* t, int32_t e[4], float st[4],
                           bool* flipOut) const {
    e[0] = static_cast<int32_t>(C1(c.w1, 12, 12));
    e[1] = static_cast<int32_t>(C1(c.w1, 0, 12));
    e[2] = static_cast<int32_t>(C0(c.w0, 12, 12));
    e[3] = static_cast<int32_t>(C0(c.w0, 0, 12));
    *flipOut = false;
    if (t == nullptr) {
        if (e[0] > e[2] || e[1] > e[3]) {
            return false;
        }
        if (e[0] == 0 && e[1] == 0) {
            NrrFillFullScreen(e);
        }
        const uint32_t mode = mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE);
        if (mode == CYC_COPY || mode == CYC_FILL) {
            e[2] += 1 << 2;
            e[3] += 1 << 2;
        }
        return true;
    }
    const bool flip = Opcode(c.w0) == OP_TEXRECTFLIP;
    int16_t step[2] = { static_cast<int16_t>(t[2].w1 >> 16), static_cast<int16_t>(t[2].w1) }; /* dsdx, dtdy */
    if (b.copy) {
        if (step[0] > 0x0400 || step[0] < -0x0400) {
            step[0] = static_cast<int16_t>(step[0] >> 2);
        }
        e[2] += 1 << 2;
        e[3] += 1 << 2;
    }
    const int16_t span[2] = { static_cast<int16_t>(e[2] - e[0]), static_cast<int16_t>(e[3] - e[1]) };
    NRR_NO_UNROLL
    for (int a = 0; a < 2; a++) {
        const int16_t origin = static_cast<int16_t>(a == 0 ? t[1].w1 >> 16 : t[1].w1); /* uls, ult */
        const int16_t d = flip ? static_cast<int16_t>(-step[a]) : step[a];
        const int16_t len = span[flip ? 1 - a : a]; /* width / height (FLIP swaps the axes) */
        st[a * 2] = static_cast<float>(origin);
        st[a * 2 + 1] = static_cast<float>(((origin << 7) + d * len) >> 7);
    }
    *flipOut = flip;
    return true;
}

/* A TEXRECT (t = the word and its two trailers) or a FILLRECT (t = nullptr) from block `b`; a
 * block that is no longer fresh (its cache slot retired, its source not checked this task, a
 * new core state generation, a stock key) is settled first (NrrBlockMiss: a re-check, a
 * re-resolve, or the stock rectangle and a recapture). */
void Interp::NrrHudHotRect(NrrBlock* b, const Cmd& c, const GdxHudWord* t, const uint32_t* op) {
    NrrHud& N = *mNrr;
    const bool fill = t == nullptr;
    if ((mNrrLate & kLateScissor) != 0 && op != nullptr) {
        const NrrWord32* sc = reinterpret_cast<const NrrWord32*>(&mRdp.scissor);
        if (GDX_UNLIKELY(((sc[0] ^ b->key.w[4]) | (sc[1] ^ b->key.w[5]) | (sc[2] ^ b->key.w[6])) != 0)) {
            b = NrrScVariant(b, c, t, fill);
            if (b == nullptr) {
                return; /* drawn the stock way (its block captured under the live key) */
            }
            if (NrrScBaseOf(b) == b) {
                const_cast<uint32_t*>(op)[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b)); /* a memo block: the op names it from now on */
            }
        }
    }
    if (b->stock != 0 || b->stGen != NrrStateGeneration() ||
        (b->usesTex != 0 && (mNrrSlotGen[b->slot] != b->slotGen || (b->imm == 0 && b->checkedSeq != N.seq)))) {
        b = NrrBlockMiss(&b->key, NrrKeyHash(b->key), b, c, t, fill);
        if (b == nullptr) {
            return;
        }
    }
    int32_t e[4];
    float st[4];
    bool flip;
    if (!NrrHudHotGeom(*b, c, t, e, st, &flip)) {
        mStats.rects++; /* == DpFillRectangle: counted, nothing drawn */
        if (GDX_UNLIKELY(N.rrCap)) {
            NrrRrCapture(op, b, nullptr); /* n60hudx nrr_hud_rr */
        }
        return;
    }
    if (fill) {
        NRR_NO_UNROLL
        for (int k = kMaxVertices; k < kMaxVertices + 4; k++) {
            mRsp.loaded_vertices[k].color = mRdp.fill_color;
        }
    }
    NrrRectMemo* m = (N.memo != nullptr && op != nullptr) ? NrrHudHotMemoFind(*b, c, t, op) : nullptr;
    float* p = NrrHudHotReserve(*b);
    if (p == nullptr) {
        if (m != nullptr) {
            m->key[0] = 1u; /* no vertices kept: never a hit (op tags are word-aligned) */
        }
        NrrNoRing(c, t, fill);
        return;
    }
    if (m != nullptr && m->key[0] == 0) {
        memcpy(p, m->out, sizeof(m->out)); /* a hit (NrrHudHotMemoFind cleared the tag word) */
        m->key[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(op));
    } else {
        NrrHudHotCorners(*b, e, st, flip, p);
        if (m != nullptr) {
            memcpy(m->out, p, sizeof(m->out));
        }
    }
    if (GDX_UNLIKELY(N.rrCap)) {
        NrrRrCapture(op, b, p); /* n60hudx nrr_hud_rr */
    }
    if (GDX_UNLIKELY(mNrrCensus >= 2)) {
        NrrDebugRect(*b, p);
    }
}

/* [native] nrr_hud_memo: everything a program rectangle's six vertices are computed from, other than
 * what the memo epoch covers (the block's fields: a capture; the corner mapping: NrrXMode; the op's
 * meaning: a program clear) -- the op (tag), the block, the rectangle's words (the opcode carries
 * the flip, the trailers the texture origin and steps), the cycle type (a FILLRECT's widening) and
 * the four corners' colour and alpha sources exactly as NrrHudHotCorners reads them. */
inline __attribute__((always_inline)) void Interp::NrrHudHotMemoKey(const NrrBlock& b, const Cmd& c,
                                                                   const GdxHudWord* t, const uint32_t* op,
                                                                   uint32_t key[kNrrMemoKeyWords]) const {
    key[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(op));
    key[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&b));
    key[2] = mNrr->memoEpoch;
    key[3] = c.w0;
    key[4] = c.w1;
    key[5] = t != nullptr ? t[1].w1 : 0u;
    key[6] = t != nullptr ? t[2].w1 : 0u;
    key[7] = (mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE)) |
             ((mRsp.extra_geometry_mode & 0x3Cu) << 8); /* the widescreen-UI x (NrrRectXFull) reads it live */
    const LoadedVertex* lv = &mRsp.loaded_vertices[kMaxVertices];
    const uint8_t* cBase = reinterpret_cast<const uint8_t*>(b.colSrc != nullptr ? b.colSrc : &lv[0].color);
    const uint8_t* aBase = reinterpret_cast<const uint8_t*>(b.alphaSrc != nullptr ? b.alphaSrc : &lv[0].color);
    const uint32_t cStep = b.colSrc != nullptr ? 0u : static_cast<uint32_t>(sizeof(LoadedVertex));
    const uint32_t aStep = b.alphaSrc != nullptr ? 0u : static_cast<uint32_t>(sizeof(LoadedVertex));
    NRR_NO_UNROLL
    for (uint32_t i = 0; i < 4; i++) {
        memcpy(&key[8 + i], cBase + i * cStep, sizeof(uint32_t));
        memcpy(&key[12 + i], aBase + i * aStep, sizeof(uint32_t));
    }
}

/* [native] nrr_hud_memo: the memo entry of a program rectangle (direct-mapped by its op), or nullptr
 * when it cannot be memoised (a block whose corner colours take the generic input switch, a
 * full-screen FILLRECT whose widening reads the frame config, a task past kRectMemos lookups: a
 * menu screen of 200+ rectangles would only thrash the table). A hit returns the entry with its tag
 * word zeroed (the caller copies the vertices and restores the tag); a miss returns it with the new
 * key stored (the caller computes the vertices, then keeps them). */
__attribute__((noinline)) Interp::NrrRectMemo* Interp::NrrHudHotMemoFind(const NrrBlock& b, const Cmd& c,
                                                                         const GdxHudWord* t, const uint32_t* op) {
    NrrHud& N = *mNrr;
    N.w.memoRects++;
    if (++N.memoTaskRects > kRectMemos || b.coldColour != 0 || (t == nullptr && C1(c.w1, 12, 12) == 0 && C1(c.w1, 0, 12) == 0)) {
        N.w.memoSkips++;
        return nullptr;
    }
    uint32_t key[kNrrMemoKeyWords];
    NrrHudHotMemoKey(b, c, t, op, key);
    const uint32_t slot = ((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(op)) >> 2) * 2654435761u) >> 25;
    NrrRectMemo& m = N.memo[slot & (kRectMemos - 1u)];
    if (memcmp(m.key, key, sizeof(key)) == 0) {
        N.w.memoHits++;
        m.key[0] = 0;
    } else {
        N.w.memoMisses++;
        memcpy(m.key, key, sizeof(key));
    }
    return &m;
}

/* ---- the per-word decoder (cold): compiles programs, finishes runs a program cannot ------------ */

/* The run, word by word, recording its program (stored when every word could be recorded as it
 * will be needed next time). */
GDX_COLD void Interp::NrrDecRun(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    const bool record = NrrNoteNoProg(w, n);
    NrrRecBegin(record && n <= kProgMaxWords && N.clock != -2);
    N.last = nullptr;
    N.dirty = true;
    for (uint32_t i = 0; i < n;) {
        i = NrrDecWord(w, n, i, N.recOk);
    }
    N.w.decRuns++;
    if (N.recOk && N.progEpoch == N.recEpoch) {
        NrrProgStore(w, n);
    } else {
        N.w.recFails++;
        if (GDX_UNLIKELY(mNiVolDiag > 0) && mNiMenuRun) {
            mNiVolDiag--;
            GDXN_LOG("[nivol] recfail ord=%u n=%u why=%u epoch=%d", mNiMenuOrd - 1u, n,
                     static_cast<unsigned>(N.recOk ? 6u : N.recWhy), N.progEpoch != N.recEpoch ? 1 : 0);
        }
    }
}

/* The receipt's split of a per-word decode: no program under the run's hash, or one whose entry
 * dependencies did not match (diagnostic; the decode itself does not depend on it). */
GDX_COLD bool Interp::NrrNoteNoProg(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    const uint32_t h = NrrHudHotHash(w, n);
    const uint32_t slot0 = NrrProgSlot(h);
    for (uint32_t p = 0; p < kProgProbe; p++) {
        const NrrProg& g = N.progs[(slot0 + p) & (kProgs - 1u)];
        if (g.hash == h && g.n == n) {
            N.w.progDepMiss++;
            if (GDX_UNLIKELY(mNrrDepDiag > 0)) {
                NrrDepDiag(g, w, n);
            }
            /* a run whose entry state keeps moving (the race intro's overlay): after a few programs
             * that did not fit, it is recorded only every 64th time it misses */
            uint16_t& m = N.recMiss[slot0 & (kRecMiss - 1u)];
            m = static_cast<uint16_t>(m + 1u);
            return m < 4u || (m & 63u) == 0;
        }
    }
    N.w.progNoHash++;
    if (GDX_UNLIKELY(mNrrDepDiag > 0)) {
        NrrNoHashDiag(w, n);
    }
    return true;
}

/* [debug] nrr_hud_depdiag (o3ds-cpucuts): a run with no program under its hash, against the last
 * such run of the same length: which words differ (the ones that move the skeleton hash). */
static GdxHudWord sNoHashLast[64];
static uint32_t sNoHashLastN, sNoHashLines;
GDX_COLD void Interp::NrrNoHashDiag(const GdxHudWord* w, uint32_t n) {
    if (n > 64u || sNoHashLines >= 4000u) {
        return;
    }
    if (n == sNoHashLastN) {
        char line[256];
        int len = snprintf(line, sizeof(line), "[nrrnohash] task=%lu n=%u diff:", static_cast<unsigned long>(mNrr->seq), n);
        uint32_t shown = 0;
        for (uint32_t i = 0; i < n && shown < 6u && len > 0 && static_cast<size_t>(len) < sizeof(line); i++) {
            if (w[i].w0 != sNoHashLast[i].w0 || w[i].w1 != sNoHashLast[i].w1) {
                len += snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " %u:%08lx_%08lx/%08lx", i,
                                static_cast<unsigned long>(w[i].w0), static_cast<unsigned long>(w[i].w1),
                                static_cast<unsigned long>(sNoHashLast[i].w1));
                shown++;
            }
        }
        sNoHashLines++;
        GDXN_LOG("%s", line);
    }
    memcpy(sNoHashLast, w, n * sizeof(GdxHudWord));
    sNoHashLastN = n;
}

/* [debug] nrr_hud_depdiag=N (n60intro): per task with dependency misses (N lines at most), how many
 * misses and which entry words differed: the scissor, the tiles, the rest. */
static uint32_t sDepMiss, sDepSc, sDepTile, sDepOther, sDepDec;
GDX_COLD void Interp::NrrDepDiag(const NrrProg& g, const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    (void)w;
    (void)n;
    const uint32_t* d = N.pool + g.deps;
    const uintptr_t sc = reinterpret_cast<uintptr_t>(&mRdp.scissor);
    const uintptr_t t0 = reinterpret_cast<uintptr_t>(&mRdp.texture_tile[0]);
    bool s = false, t = false, o = false;
    for (uint32_t k = 0; k < g.nDeps; k++, d += 3) {
        const uint32_t live = *reinterpret_cast<const NrrWord32*>(d[0]);
        if (((live ^ d[2]) & d[1]) != 0) {
            const uintptr_t a = static_cast<uintptr_t>(d[0]);
            if (a + 4 > sc && a < sc + sizeof(Rect)) {
                s = true;
            } else if (a + 4 > t0 && a < t0 + sizeof(mRdp.texture_tile)) {
                t = true;
            } else {
                o = true;
            }
        }
    }
    sDepMiss++;
    sDepSc += s ? 1u : 0u;
    sDepTile += t ? 1u : 0u;
    sDepOther += o ? 1u : 0u;
}

GDX_COLD void Interp::NrrDepDiagTask() {
    NrrHud& N = *mNrr;
    if (N.w.decRuns < sDepDec) {
        sDepDec = 0; /* the receipt window restarted */
    }
    if (sDepMiss != 0 || N.w.decRuns != sDepDec) {
        mNrrDepDiag--;
        GDXN_LOG("[nrrdep] task=%lu misses=%lu scissor=%lu tiles=%lu other=%lu decruns=%lu", static_cast<unsigned long>(N.seq),
                 static_cast<unsigned long>(sDepMiss), static_cast<unsigned long>(sDepSc),
                 static_cast<unsigned long>(sDepTile), static_cast<unsigned long>(sDepOther),
                 static_cast<unsigned long>(N.w.decRuns - sDepDec));
    }
    sDepMiss = sDepSc = sDepTile = sDepOther = 0;
    sDepDec = N.w.decRuns;
}

/* The rest of a run from word `i`, nothing recorded. */
GDX_COLD void Interp::NrrDecFrom(const GdxHudWord* w, uint32_t n, uint32_t i) {
    NrrHud& N = *mNrr;
    N.last = nullptr;
    N.dirty = true;
    N.w.progAbandons++;
    while (i < n) {
        i = NrrDecWord(w, n, i, false);
    }
}

/* An EX op: one word through the per-word decoder. Returns the next word's index. A rectangle it
 * draws from a block (one the compile had to draw the stock way) means the program can do better:
 * it goes, and the run records a new one next time. */
GDX_COLD uint32_t Interp::NrrProgEx(const NrrProg& p, const GdxHudWord* w, uint32_t n, uint32_t i) {
    NrrHud& N = *mNrr;
    N.last = nullptr;
    N.dirty = true;
    N.decFast = false;
    N.w.progEx++;
    const uint32_t next = NrrDecWord(w, n, i, false);
    if (N.decFast && p.n != 0) {
        const_cast<NrrProg&>(p).n = 0;
        N.nProgs--;
        N.w.progMiss++;
    }
    return next;
}

/* A program that does not fit the run (a word differs from the one it was compiled from, or the
 * texture at a rectangle's TMEM address): it goes (the run records a new one next time), and the
 * per-word decoder finishes the run from word `i` (every op before it fitted). */
GDX_COLD void Interp::NrrProgMiss(const NrrProg& p, const GdxHudWord* w, uint32_t n, uint32_t i) {
    const_cast<NrrProg&>(p).n = 0;
    mNrr->nProgs--;
    mNrr->w.progMiss++;
    NrrDecFrom(w, n, i);
}

/* Append one op to the program being recorded: its header, the word it was compiled from, its
 * payload. */
GDX_COLD void Interp::NrrRec(uint32_t hdr, const Cmd& c, const uint32_t* payload, uint32_t words) {
    NrrHud& N = *mNrr;
    if (N.recN + 4u + words > kRecWords) {
        N.recOk = false;
        N.recWhy = 1;
        return;
    }
    N.rec[N.recN++] = hdr;
    N.rec[N.recN++] = c.w0;
    N.rec[N.recN++] = c.w1;
    N.rec[N.recN++] = c.w1hi;
    for (uint32_t k = 0; k < words; k++) {
        N.rec[N.recN++] = payload[k];
    }
}

/* A word the per-word decoder runs again at runtime (a stock rectangle, a real load, a triangle):
 * it re-decides from the live state, so it adds no entry dependency. */
GDX_COLD void Interp::NrrRecEx(uint32_t i) {
    NrrRec(kOpEx | (i << 16), Cmd{ 0, 0, 0 }, nullptr, 0);
}

/* A word whose effect on the registers is not known (a handler that is not a pure masked write):
 * the program then depends on every snapshot word. */
GDX_COLD void Interp::NrrRecOpaque(uint32_t i) {
    NrrHud& N = *mNrr;
    if (N.opaqueLogs < 16) {
        N.opaqueLogs++;
        GDXN_LOG("[nrrhud] opaque word %lu: %08lx %08lx", static_cast<unsigned long>(i),
                 static_cast<unsigned long>(N.runWords[i].w0), static_cast<unsigned long>(N.runWords[i].w1));
    }
    mNrr->recFull = true;
    NrrRecEx(i);
}

/* Start recording a run's program (`ok`: it may be stored): the entry snapshot, no reads / writes. */
GDX_COLD void Interp::NrrRecBegin(bool ok) {
    NrrHud& N = *mNrr;
    N.recN = 0;
    N.recOk = ok;
    N.recWhy = ok ? 0 : 7;
    N.recFull = false;
    N.recEpoch = N.progEpoch;
    if (ok) {
        NrrSnapTake(N.recSnap);
        memset(N.recRead, 0, sizeof(N.recRead));
        memset(N.recWrote, 0, sizeof(N.recWrote));
    }
}

/* The snapshot index of the word holding byte `p` (kSnapMax: not a snapshot word). */
GDX_COLD uint32_t Interp::NrrSnapIndex(const void* p) const {
    const NrrHud& N = *mNrr;
    const NrrWord32* wp = reinterpret_cast<const NrrWord32*>(reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(3));
    uint32_t lo = 0, hi = N.nSnap; /* snapPtr is sorted (NrrSnapInit) */
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (N.snapPtr[mid] < wp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo < N.nSnap && N.snapPtr[lo] == wp) ? lo : kSnapMax;
}

/* Bytes [p, p + len) read by the run: entry dependencies, unless the run wrote them first. */
GDX_COLD void Interp::NrrRecRead(const void* p, uint32_t len) {
    NrrHud& N = *mNrr;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    for (uintptr_t byte = a; byte < a + len; byte++) {
        const uint32_t k = NrrSnapIndex(reinterpret_cast<const void*>(byte));
        if (k != kSnapMax) {
            N.recRead[k] |= (0xFFu << (8u * (byte & 3u))) & N.snapMask[k] & ~N.recWrote[k];
        }
    }
}

/* The run wrote `mask` of the word at `p` (an ST store, the TLUT identity). */
GDX_COLD void Interp::NrrRecWrote(const void* p, uint32_t mask) {
    NrrHud& N = *mNrr;
    const uint32_t k = NrrSnapIndex(p);
    if (k != kSnapMax) {
        N.recWrote[k] |= mask & N.snapMask[k];
    }
}

GDX_COLD void Interp::NrrRecSt(uint32_t i, const Cmd& c, uint32_t hdr, const uint32_t* st) {
    const uint32_t n = (hdr >> 4) & 15u;
    for (uint32_t k = 0; k < n; k++) {
        NrrRecWrote(reinterpret_cast<const void*>(static_cast<uintptr_t>(st[k * 3])), st[k * 3 + 1]);
    }
    NrrRec((hdr & 0xFFFFu) | (i << 16), c, st, 3u * n);
}

/* An LD op: its record reads SETTIMG's operand and the load tile's TMEM address. */
GDX_COLD void Interp::NrrRecLd(uint32_t i, const uint32_t* r, const Cmd& c) {
    const auto& tl = mRdp.texture_to_load;
    NrrRecRead(&tl.width, sizeof(tl.width));
    NrrRecRead(&tl.siz, 1);
    NrrRecRead(&tl.cmd, sizeof(tl.cmd));
    NrrRecRead(&mRdp.texture_tile[C1(c.w1, 24, 3)].tmem_index, sizeof(uint16_t));
    if (tl.width > 1 && (mNrrLate & kLatePitch) == 0) {
        NrrRecWideReads(static_cast<uint8_t>(C1(c.w1, 24, 3))); /* else checked when it runs (NrrLdPitchOk) */
    }
    NrrRec(kOpLd | (i << 16), c, r, kLdWords);
}

/* A wide load's line pitch (LoadBlockLineBytes) reads every other tile's TMEM address, pitch and
 * texel size: entry dependencies of its LD op unless the run wrote them first. */
GDX_COLD void Interp::NrrRecWideReads(uint8_t loadTile) {
    for (uint32_t k = 0; k < 8; k++) {
        if (k == loadTile) {
            continue;
        }
        const Tile& t = mRdp.texture_tile[k];
        NrrRecRead(&t.tmem_index, sizeof(t.tmem_index));
        NrrRecRead(&t.line_size_bytes, sizeof(t.line_size_bytes));
        NrrRecRead(&t.siz, sizeof(t.siz));
    }
}

/* A TLUT word (run again at runtime): its identity note reads SETTIMG's operand and the tile's TMEM
 * address, and writes the TLUT identities. */
GDX_COLD void Interp::NrrRecTl(uint32_t i, const Cmd& c) {
    NrrHud& N = *mNrr;
    NrrRecRead(&mRdp.texture_to_load.cmd, sizeof(Cmd));
    NrrRecRead(&mRdp.texture_tile[C1(c.w1, 24, 3)].tmem, sizeof(uint16_t));
    NrrRecWrote(&N.tlut[0], 0xFFFFFFFFu);
    NrrRecWrote(&N.tlut[1], 0xFFFFFFFFu);
    NrrRecEx(i);
}

/* An RT / FL op: the block its key named; the key's register reads; a TEXRECT's texture identity
 * (checked at runtime against the live identity at the tile's TMEM address). */
GDX_COLD void Interp::NrrRecRect(uint32_t i, const Cmd& c, const NrrBlock* b, uint8_t tile, bool fill) {
    NrrHud& N = *mNrr;
    NrrRecRead(&mRdp.combine_mode, sizeof(mRdp.combine_mode));
    NrrRecRead(&mRdp.other_mode_h, sizeof(mRdp.other_mode_h));
    NrrRecRead(&mRdp.other_mode_l, sizeof(mRdp.other_mode_l));
    if ((mNrrLate & kLateScissor) == 0) {
        NrrRecRead(&mRdp.scissor, sizeof(mRdp.scissor)); /* else NrrHudHotRect takes the live one */
    }
    NrrRecRead(&mRdp.blend_color.a, 1);
    NrrRecRead(&mRdp.grayscale, 1);
    uint32_t pay[kRtWords];
    pay[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b));
    if (fill) {
        NrrRecRead(&mRdp.first_tile_index, 1); /* (the depth-clear test is the FL op's own, at runtime) */
        NrrRec(kOpFl | (i << 16), Cmd{ c.w0, 0, 0 }, pay, 1);
        return;
    }
    const Tile& t = mRdp.texture_tile[tile];
    NrrRecRead(&t, sizeof(Tile));
    if (t.fmt == IM_FMT_CI) {
        NrrRecRead(&N.tlut[0], sizeof(N.tlut));
    }
    for (uint32_t k = 0; k < 4; k++) {
        pay[1 + k] = b->key.w[17 + k]; /* the identity the key was built from */
    }
    NrrRec(kOpRt | (i << 16), Cmd{ c.w0, c.w1, 0 }, pay, kRtWords);
}

/* One word (TEXRECT: with its two trailers); returns the next word's index. */
GDX_COLD uint32_t Interp::NrrDecWord(const GdxHudWord* w, uint32_t n, uint32_t i, bool rec) {
    const Cmd c = { w[i].w0, w[i].w1, w[i].w1hi };
    const uint8_t op = Opcode(c.w0);
    if ((op == OP_TEXRECT || op == OP_TEXRECTFLIP) && i + 2 < n) {
        NrrDecRect(c, w + i, i, rec);
        return i + 3;
    }
    if (op == OP_FILLRECT) {
        NrrDecRect(c, nullptr, i, rec);
        return i + 1;
    }
    if (op == OP_LOADBLOCK) {
        NrrDecLoad(c, i, rec);
        return i + 1;
    }
    if (op == OP_DL) {
        return NrrDecDl(w, n, i, rec, nullptr); /* n60hudx nrr_hud_kept */
    }
    if (GDX_UNLIKELY(mNiMenuRun) && op == OP_LOADTILE) {
        const uint32_t next = NrrDecReg(c, i, rec); /* the real load, an opaque op (as before) */
        NrrNoteLoadTile(c);                          /* nimenu: and the identity it leaves */
        return next;
    }
    return NrrDecReg(c, i, rec);
}

/* ---- n60hudx [native] nrr_hud_kept: a G_DL inside the run (the capture recorded it) -------------- */

/* An SL op that does not fit (why: 1 the list no longer resolves, 2 the G_DL hook drew it -- either
 * way Exec would not enter it, and this one already resolved / offered it --, 3 another list or other
 * words): the program goes and the per-word decoder finishes the run from the G_DL, with the list
 * already resolved. */
GDX_COLD void Interp::NrrSlMiss(const NrrProg& p, const GdxHudWord* w, uint32_t n, uint32_t i, const ListRef& sub,
                                int why) {
    NrrHud& N = *mNrr;
    const_cast<NrrProg&>(p).n = 0;
    N.nProgs--;
    N.w.progMiss++;
    N.w.slMiss++;
    uint32_t next = i + 1;
    if (why == 3) {
        next = NrrDecDl(w, n, i, false, &sub);
    }
    NrrDecFrom(w, n, next);
}

/* A G_DL in the run, per word: Exec's resolve and hook (unless `pre`: resolved and offered already,
 * NrrSlMiss); a static setup list of pure register words (NrrSlCompile) runs through its words'
 * compiled stores, which the program records as an SL op (every store a write of the run); any other
 * list is walked in place by the walker's own loop (WalkNestedList) -- exactly what the walker did
 * when the G_DL was a kept word between two runs, the end of the run before it included -- and the run
 * gets no program (the list's effect on the registers is not known). */
GDX_COLD uint32_t Interp::NrrDecDl(const GdxHudWord* w, uint32_t n, uint32_t i, bool rec, const ListRef* pre) {
    NrrHud& N = *mNrr;
    const Cmd c = { w[i].w0, w[i].w1, w[i].w1hi };
    N.last = nullptr;
    N.dirty = true;
    N.w.slDecs++;
    if (N.drew) {
        mRdp.viewport_or_scissor_changed = true; /* == the run before the G_DL ending (NrrHudHotBody) */
        N.drew = false;
    }
    ListRef sub;
    if (pre != nullptr) {
        sub = *pre;
    } else {
        bool taken = false;
        if (!ResolveDlAsExec(c, sub, &taken) || taken) {
            if (GDX_UNLIKELY(mNiMenu != 0) && c.w1hi == kHostTag) {
                gdxhud_kept_refuse(c.w1); /* nimenu: a list the G_DL hook draws (machines) stays in place */
            }
            if (rec) {
                N.recOk = false;
                N.recWhy = 2;
                NrrRecEx(i);
            }
            return i + 1;
        }
    }
    uint32_t words = 0;
    const bool plain = mBufNumTris == 0 && mTexOv == nullptr;
    NrrHud::SlEnt& ce = N.slc[((c.w1 >> 3) ^ (c.w1 >> 9)) & (kSlCache - 1u)];
    if (plain && ce.words != 0 && ce.w0 == c.w0 && ce.w1 == c.w1 && ce.w1hi == c.w1hi) {
        /* compiled before: the list must still be the one compiled (== the SL op's check) */
        const uint32_t ref = static_cast<uint32_t>(sub.stride) | (sub.isBig ? 0x100u : 0u) | (sub.isF3d ? 0x200u : 0u);
        if (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sub.base)) == ce.pay[0] &&
            static_cast<uint32_t>(sub.limit) == ce.pay[1] && ref == ce.pay[2] &&
            memcmp(sub.base, ce.pay + 4, (ce.pay[3] & 0xFFFFu) * sub.stride) == 0) {
            NrrHudHotGroups(ce.pay + 4 + NrrSlRawWords(ce.pay), ce.pay + ce.words);
            words = ce.words;
            memcpy(N.slBuf, ce.pay, words * sizeof(uint32_t));
            N.w.slHits++;
        }
    }
    const bool cached = words != 0;
    const int r = cached ? 1 : plain ? NrrSlCompile(sub, i, N.slBuf, &words) : 0;
    if (r == 1 && words <= kSlCacheWords && (ce.words == 0 || ce.w1 != c.w1 || ce.w0 != c.w0 || ce.w1hi != c.w1hi ||
                                             memcmp(ce.pay, N.slBuf, words * sizeof(uint32_t)) != 0)) {
        ce.w0 = c.w0;
        ce.w1 = c.w1;
        ce.w1hi = c.w1hi;
        ce.words = words;
        memcpy(ce.pay, N.slBuf, words * sizeof(uint32_t));
    }
    if (r == 1) {
        N.w.slBuilds += cached ? 0u : 1u;
        if (rec) {
            const uint32_t* g = N.slBuf + 4 + NrrSlRawWords(N.slBuf);
            const uint32_t* const end = N.slBuf + words;
            while (g < end) {
                const uint32_t ns = (g[0] >> 4) & 15u;
                for (uint32_t k = 0; k < ns; k++) {
                    NrrRecWrote(reinterpret_cast<const void*>(static_cast<uintptr_t>(g[1 + 3 * k])), g[2 + 3 * k]);
                }
                g += 1u + 3u * ns;
            }
            NrrRec(kOpSl | (i << 16), c, N.slBuf, words);
        }
        return i + 1;
    }
    if (GDX_UNLIKELY(mNiMenu != 0) && c.w1hi == kHostTag) {
        gdxhud_kept_refuse(c.w1); /* nimenu: a MENU region keeps this G_DL in place from now on */
    }
    if (r == 0) {
        N.w.slWalks++;
        WalkNestedList(sub); /* == the walker entering it between two runs */
        if (N.atlasEpoch != *N.atlasPtr || N.envSeq != N.seq || N.envEgm != mRsp.extra_geometry_mode) {
            NrrRunEpochs(); /* == the next run's start (NrrHudHotBody) */
        }
        if (mBufNumTris != 0 && (mNrrLate & kLateFlush) != 0 && mTexOv == nullptr) {
            NrrLateFlush(); /* == the next run's start (NrrHudHotBody) */
        }
        N.last = nullptr;
        N.dirty = true;
    }
    if (rec) {
        N.recOk = false;
        N.recWhy = static_cast<uint8_t>(r == 0 ? 3 : 8); /* 3 a walked (impure) list, 8 another refusal */
        NrrRecEx(i);
    }
    return i + 1;
}

/* A setup list as an SL op's payload, its words executed (compiled: each runs its handler for real,
 * NrrRegCompile). Returns 0 when the list is not one (an F3D list, a word that is not a compilable
 * register word / a sync, no terminator within kSlMaxCmds): nothing ran. 1: compiled, every word ran.
 * 2: every word ran but one did not compile (exec-only): no SL op.
 * The payload's ST groups: the stores of consecutive words whose handler has no conditional effect
 * (no texture arm, no empty-flush count) merge into one group, one masked store per register word
 * (a later store over an earlier one: masks or'ed, the later value wins where it writes -- the
 * same register bits as the stores in order). A word with a conditional effect keeps its own group
 * (its "changed" test is its own stores'). G_TEXTURE's handler arms the texture flag only when the
 * first tile changes: its first-tile store is such a group of its own, its other stores merge. */
GDX_COLD int Interp::NrrSlCompile(const ListRef& sub, uint32_t i, uint32_t* pay, uint32_t* words) {
    if (sub.isF3d || sub.base == nullptr) {
        return 0;
    }
    uint32_t nCmd = 0;
    bool term = false;
    for (size_t j = 0; j < sub.limit && j < kSlMaxCmds && !term; j++) {
        const uint8_t o = Opcode(ReadCmd(sub, j).w0);
        nCmd = static_cast<uint32_t>(j + 1);
        if (o == OP_ENDDL) {
            term = true;
        } else if (o != OP_RDPLOADSYNC && o != OP_RDPPIPESYNC && o != OP_RDPTILESYNC && o != OP_RDPFULLSYNC &&
                   !NrrRegCompilable(o) && o != OP_TEXTURE) {
            return 0;
        }
    }
    if (!term) {
        return 0;
    }
    pay[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sub.base));
    pay[1] = static_cast<uint32_t>(sub.limit);
    pay[2] = static_cast<uint32_t>(sub.stride) | (sub.isBig ? 0x100u : 0u) | (sub.isF3d ? 0x200u : 0u);
    if ((sub.stride != 8 && sub.stride != 16) || 4u + nCmd * (sub.stride / 4u) + 2u * (1u + 3u * kFxStores) > kSlPayMax) {
        return 0;
    }
    pay[3] = nCmd;
    memcpy(&pay[4], sub.base, nCmd * sub.stride); /* the list's words as they are in memory */
    uint32_t at = 4u + NrrSlRawWords(pay);
    const uintptr_t ftWord = reinterpret_cast<uintptr_t>(&mRdp.first_tile_index) & ~static_cast<uintptr_t>(3);
    /* the open merged group: pay[mg] is its header, mn stores after it */
    uint32_t mg = 0, mn = 0;
    bool ok = true;
    for (uint32_t j = 0; j + 1 < nCmd; j++) {
        const Cmd s = ReadCmd(sub, j);
        const uint8_t o = Opcode(s.w0);
        if (o == OP_RDPLOADSYNC || o == OP_RDPPIPESYNC || o == OP_RDPTILESYNC || o == OP_RDPFULLSYNC) {
            continue; /* no-ops (Exec) */
        }
        if (!ok) {
            (void)NrrExecWord(s, i);
            continue;
        }
        NrrRegFx fx;
        NrrRegCompile(fx, s, i); /* the handler runs for real */
        if (fx.n > kFxStores || at + 2u * (1u + 3u * kFxStores) > kSlPayMax) {
            ok = false;
            continue;
        }
        uint32_t st[3 * kFxStores];
        const uint32_t hdr = NrrFxHeader(fx, st);
        const bool own = fx.arm != 0 || fx.why != FLUSH_NONE;
        for (uint32_t k = 0; k < fx.n; k++) {
            const bool ft = o == OP_TEXTURE && static_cast<uintptr_t>(st[3 * k]) == ftWord;
            if (own || ft) {
                continue; /* below, in a group of its own */
            }
            uint32_t e = 0;
            for (; e < mn && pay[mg + 1 + 3 * e] != st[3 * k]; e++) {
            }
            if (mn != 0 && e < mn) {
                pay[mg + 2 + 3 * e] |= st[3 * k + 1];
                pay[mg + 3 + 3 * e] = (pay[mg + 3 + 3 * e] & ~st[3 * k + 1]) | st[3 * k + 2];
                continue;
            }
            if (mn == 0 || mn == 15u) {
                mg = at++;
                mn = 0;
            }
            pay[at++] = st[3 * k];
            pay[at++] = st[3 * k + 1];
            pay[at++] = st[3 * k + 2];
            mn++;
            pay[mg] = kOpSt | (mn << 4);
        }
        if (own) {
            pay[at++] = hdr;
            for (uint32_t k = 0; k < 3u * fx.n; k++) {
                pay[at++] = st[k];
            }
            mn = 0; /* a later word's stores may not move across this group */
        } else if (o == OP_TEXTURE) {
            uint32_t cnt = 0;
            const uint32_t h = at++;
            for (uint32_t k = 0; k < fx.n; k++) {
                if (static_cast<uintptr_t>(st[3 * k]) == ftWord) {
                    pay[at++] = st[3 * k];
                    pay[at++] = st[3 * k + 1];
                    pay[at++] = st[3 * k + 2];
                    cnt++;
                }
            }
            if (cnt == 0) {
                at--;
            } else {
                pay[h] = kOpSt | (cnt << 4) | kOpArm;
                mn = 0;
            }
        }
    }
    pay[3] = nCmd | (at << 16);
    *words = at;
    return ok ? 1 : 2;
}

/* A compiled word's stores into the live registers (the per-word decoder; == an ST op). */
GDX_COLD bool Interp::NrrRegApply(const NrrRegFx& fx) {
    uint32_t st[3 * kFxStores];
    const uint32_t hdr = NrrFxHeader(fx, st);
    const uint32_t why = (hdr >> 9) & 31u;
    const uint32_t flush0 = mStats.flush[why];
    (void)NrrHudHotStores(hdr, st);
    return mStats.flush[why] != flush0;
}

/* A register word: its compiled stores (ST), or its handler (EX: an uncompilable word, a word
 * with an interpreter batch open, a TLUT / triangle / extra geometry word: NrrColdWord). */
GDX_COLD uint32_t Interp::NrrDecReg(const Cmd& c, uint32_t i, bool rec) {
    NrrHud& N = *mNrr;
    uint32_t col[3];
    if (mBufNumTris == 0 && mTexOv == nullptr && NrrColourOp(c, col)) {
        /* a colour word: its fixed effect (a COL op), never compiled -- the finish screen's text
         * field alone sets ~200 distinct prim colours a frame (nrr_hud_pd: a prim depth too) */
        const uint32_t hdr = NrrColourHdr(Opcode(c.w0)) | (i << 16);
        NrrHudHotColour(hdr, col, c);
        if (rec) {
            NrrRec(hdr, Cmd{ c.w0, 0, c.w1hi }, col, 3);
        }
        return i + 1;
    }
    uint32_t h = (c.w0 * 0x9E3779B1u) ^ (c.w1 * 0x85EBCA77u);
    h ^= h >> 15;
    const bool plain = mBufNumTris == 0 && mTexOv == nullptr;
    const NrrRegFx* fx = nullptr;
    for (uint32_t p = 0; p < kRegProbe && fx == nullptr; p++) {
        const NrrRegFx& e = N.regs[(h + p) & (kRegFx - 1u)];
        if (e.used == 0) {
            break;
        }
        if (e.w0 == c.w0 && e.w1 == c.w1 && e.w1hi == c.w1hi) {
            fx = &e;
        }
    }
    uint32_t next = i + 1;
    if (fx != nullptr && fx->n <= kFxStores && plain) {
        (void)NrrRegApply(*fx);
        N.dirty = N.dirty || fx->dirty != 0;
    } else {
        fx = nullptr;
        next = NrrRegCold(c, i, h, &fx);
    }
    if (rec) {
        NrrRecRegWord(c, i, fx, plain);
    }
    return next;
}

/* A compilable word the memo holds only as seen once (or not at all): it compiles at its next sight. */
GDX_COLD bool Interp::NrrRegFirstSight(const Cmd& c) const {
    const NrrHud& N = *mNrr;
    uint32_t h = (c.w0 * 0x9E3779B1u) ^ (c.w1 * 0x85EBCA77u);
    h ^= h >> 15;
    for (uint32_t p = 0; p < kRegProbe; p++) {
        const NrrRegFx& e = N.regs[(h + p) & (kRegFx - 1u)];
        if (e.used == 0) {
            return true;
        }
        if (e.w0 == c.w0 && e.w1 == c.w1 && e.w1hi == c.w1hi) {
            return e.n == kFxSeen;
        }
    }
    return true;
}

/* A SETPRIMCOLOR / SETENVCOLOR as a COL op, whatever its colour: payload {colour register word, prim's
 * lod-fraction word or 0, the lod byte's shift}. The handler's effect (RgbaOf(w1) into the
 * register, w0's low byte into prim_lod_fraction, NoteRegChange on a change) is fixed by its code;
 * nrr_hud=2 checks every COL op against the handler. false: the registers are not word aligned. */
GDX_COLD bool Interp::NrrColourOp(const Cmd& c, uint32_t col[3]) const {
    const uint8_t op = Opcode(c.w0);
    if (op == OP_SETPRIMDEPTH && sNrrHudPdOn) {
        return NrrPrimDepthOp(col);
    }
    if (op != OP_SETPRIMCOLOR && op != OP_SETENVCOLOR) {
        return false;
    }
    const Rgba* reg = op == OP_SETPRIMCOLOR ? &mRdp.prim_color : &mRdp.env_color;
    const uintptr_t lodAt = reinterpret_cast<uintptr_t>(&mRdp.prim_lod_fraction);
    if ((reinterpret_cast<uintptr_t>(reg) & 3u) != 0) {
        return false;
    }
    col[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reg));
    col[1] = op == OP_SETPRIMCOLOR ? static_cast<uint32_t>(lodAt & ~static_cast<uintptr_t>(3)) : 0u;
    col[2] = static_cast<uint32_t>((lodAt & 3u) * 8u);
    return true;
}

/* nrr_hud_pd: a SETPRIMDEPTH's COL payload {prim_depth's register word, 0, its shift}. false: the
 * 16-bit register straddles a word (it cannot: it is 2-aligned). */
GDX_COLD bool Interp::NrrPrimDepthOp(uint32_t col[3]) const {
    const uintptr_t at = reinterpret_cast<uintptr_t>(&mRdp.prim_depth);
    if ((at & 1u) != 0) {
        return false;
    }
    col[0] = static_cast<uint32_t>(at & ~static_cast<uintptr_t>(3));
    col[1] = 0;
    col[2] = static_cast<uint32_t>((at & 3u) * 8u);
    return true;
}

/* A COL op's header (no word index): its flush reason, the prim-depth form's flag. */
uint32_t Interp::NrrColourHdr(uint8_t op) {
    if (op == OP_SETPRIMDEPTH) {
        return kOpCol | kOpColPd | (static_cast<uint32_t>(FLUSH_PRIMDEPTH) << 9);
    }
    const uint32_t why = op == OP_SETPRIMCOLOR ? FLUSH_PRIM : FLUSH_ENV;
    return kOpCol | (why << 9);
}

/* What a register word records: ST (compiled stores), TL (a TLUT), EX (a triangle, extra geometry,
 * a pure word that ran with an interpreter batch open), or an opaque word. */
GDX_COLD void Interp::NrrRecRegWord(const Cmd& c, uint32_t i, const NrrRegFx* fx, bool plain) {
    const uint8_t op = Opcode(c.w0);
    uint32_t col[3];
    if (plain && NrrColourOp(c, col)) {
        NrrRec(NrrColourHdr(op) | (i << 16), Cmd{ c.w0, 0, c.w1hi }, col, 3);
        return;
    }
    if (fx != nullptr && plain) {
        uint32_t st[3 * kFxStores];
        const uint32_t hdr = NrrFxHeader(*fx, st);
        NrrRecSt(i, c, hdr, st);
    } else if (NrrRegCompilable(op) && plain && NrrRegFirstSight(c)) { /* (colours took COL above) */
        mNrr->recOk = false; /* not compiled yet (first sight): the program is recorded on a later run */
        mNrr->recWhy = 4;
        NrrRecEx(i);
    } else if (op == OP_LOADTLUT) {
        NrrRecTl(i, c);
    } else if (NrrRegCompilable(op) && !plain) {
        mNrr->recOk = false; /* an interpreter batch is open: a program needs none (it never runs then) */
        mNrr->recWhy = 5;
        NrrRecEx(i);
    } else if (op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD || op == OP_EXTRAGEOMETRYMODE) {
        NrrRecEx(i);
    } else {
        NrrRecOpaque(i);
    }
}

/* A LOADBLOCK: virtual (a host-operand G_LOADBLOCK form -- SETTIMG width 1: DpLoadBlock's other-tile
 * line search never runs, so the load is a function of its own words and the load tile's TMEM
 * address -- recorded as LD), else for real (EX). */
GDX_COLD void Interp::NrrDecLoad(const Cmd& c, uint32_t i, bool rec) {
    NrrHud& N = *mNrr;
    uint32_t r[kLdWords];
    N.dirty = true;
    if (!NrrLoadRecord(c, r)) {
        NrrLoadCold(c);
        if (rec) {
            NrrRecEx(i);
        }
        return;
    }
    NrrHudHotLoad(r, c);
    if (rec) {
        NrrRecLoad(i, r, c);
    }
}

/* What a virtual load records: an LD op (its record is a function of the run's entry state), or --
 * a segmented operand, resolved through the segment table, which no snapshot word covers -- EX
 * (the per-word decoder resolves it again at runtime; the next RT op's identity check catches a
 * moved segment). */
GDX_COLD void Interp::NrrRecLoad(uint32_t i, const uint32_t* r, const Cmd& c) {
    if (NrrSegResolved(r)) {
        NrrRecEx(i);
    } else {
        NrrRecLd(i, r, c);
    }
}

/* The virtual-load record of a LOADBLOCK under the live registers; false = not a virtual form. */
GDX_COLD bool Interp::NrrLoadRecord(const Cmd& c, uint32_t r[kLdWords]) {
    const auto& tl = mRdp.texture_to_load;
    const uint8_t loadTile = static_cast<uint8_t>(C1(c.w1, 24, 3));
    const uint32_t lo = mRdp.texture_tile[loadTile].tmem_index;
    const bool host = tl.cmd.w1hi == kHostTag;
    if ((tl.width > 1 && (mNrrWide & kWideLoads) == 0) || (!host && (mNrrWide & kWideSegLoads) == 0) ||
        lo >= 512u) {
        return false;
    }
    const uint32_t bytes = NrrTexelBytes(C1(c.w1, 12, 12) + 1u, tl.siz);
    const uint32_t words = (bytes + 7u) >> 3;
    const uint32_t hi = lo + words < 512u ? lo + words : 512u;
    uint32_t line = bytes; /* DpLoadBlock's pitch of a width-1 transfer: the whole block */
    r[0] = tl.cmd.w0;
    if (tl.width > 1) {
        line = LoadBlockLineBytes(loadTile, static_cast<uint16_t>(lo), bytes, C1(c.w1, 0, 12));
        if (line == 0 || line > kWideLineMax) {
            return false;
        }
        r[0] = NrrWideTimg(tl.cmd.w0, line);
    }
    r[1] = tl.cmd.w1;
    r[2] = c.w0;
    r[3] = c.w1;
    r[4] = lo | (hi << 16);
    return host || NrrSegResolve(c, line, bytes, r);
}

/* A segmented operand ([native] nrr_hud_wide bit 4), resolved now -- the moment DpLoadBlock would
 * resolve it, over the extent it would read -- and named by the pointer it resolved to (the
 * record's SETTIMG w1) and the classification the resolution made (immutable, host-order RGBA16:
 * the LOADBLOCK w0's opcode byte). NrrMaterializeAt hands both back (mLoadSrcForce), so the load
 * reads the bytes the stock one would have, classified as it would have. */
GDX_COLD bool Interp::NrrSegResolve(const Cmd& c, uint32_t line, uint32_t bytes, uint32_t r[kLdWords]) {
    const auto& tl = mRdp.texture_to_load;
    const size_t need = static_cast<size_t>(C0(c.w0, 0, 12)) * line + NrrTexelBytes(C0(c.w0, 12, 12), tl.siz) + bytes;
    if (!ResolveTextureSource(need > 0 ? need : 1)) {
        return false;
    }
    const uint32_t flags = kSegResolved | (tl.immutable != 0 ? kSegImmutable : 0u) | (tl.native_rgba16 != 0 ? kSegNative : 0u);
    r[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tl.addr));
    r[2] = (c.w0 & 0x00FFFFFFu) | (flags << 24);
    return true;
}

/* A rectangle: its block (the key, the memo: NrrDecBlockFor), drawn from it (RT / FL), or the
 * stock rectangle (EX unless its key now has a usable block: a first sight captured one). */
GDX_COLD void Interp::NrrDecRect(const Cmd& c, const GdxHudWord* t, uint32_t i, bool rec) {
    const bool fill = t == nullptr;
    if (fill && mRdp.color_image_address == mRdp.z_buf_address) {
        mNrr->w.rects++;
        (void)NrrColdWord(c, i); /* DpFillRectangle's depth-clear arm */
        if (rec) {
            NrrRecEx(i);
        }
        return;
    }
    const uint8_t tile = fill ? mRdp.first_tile_index : static_cast<uint8_t>(C1(c.w1, 24, 3));
    NrrBlock* b = NrrDecBlockFor(tile, fill, c, t);
    if (b != nullptr) {
        mNrr->decFast = true;
        NrrHudHotRect(b, c, t, nullptr);
    } else {
        mNrr->w.rects++;
    }
    if (!rec) {
        return;
    }
    if (b == nullptr) {
        NrrKey k;
        uint32_t h = 0;
        b = NrrDecKey(k, tile, fill) ? NrrDecFind(k, &h) : nullptr; /* captured just now? */
        if (b != nullptr && (b->stock != 0 || b->stGen != NrrStateGeneration())) {
            b = nullptr;
        }
    }
    if (b != nullptr && (mNrrLate & kLateScissor) != 0) {
        b = NrrScBaseOf(b); /* a program names memo blocks only (its rectangles take the live scissor) */
    }
    if (b == nullptr) {
        NrrRecEx(i);
        return;
    }
    NrrRecRect(i, c, b, tile, fill);
}

/* The key of a rectangle drawn from `tile` (a FILLRECT: no texture identity). false = the
 * texture's identity is unknown to the NRR (the stock rectangle draws it). */
GDX_COLD bool Interp::NrrDecKey(NrrKey& key, uint8_t tile, bool fill) const {
    const NrrHud& N = *mNrr;
    uint32_t* k = key.w;
    const uint32_t cycle = mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE);
    uint64_t comb = mRdp.combine_mode;
    uint32_t omh = mRdp.other_mode_h;
    if (cycle == CYC_COPY) {
        omh = (omh & ~(3u << MDSFT_TEXTFILT)) | TF_POINT;
        comb = fill ? comb : kCopyCombine;
    } else if (fill && cycle == CYC_FILL) {
        comb = kFillCombine;
    }
    k[0] = static_cast<uint32_t>(comb);
    k[1] = static_cast<uint32_t>(comb >> 32);
    k[2] = omh;
    k[3] = mRdp.other_mode_l;
    const NrrWord32* sc = reinterpret_cast<const NrrWord32*>(&mRdp.scissor);
    const Tile& t = mRdp.texture_tile[tile];
    const NrrWord32* tw = reinterpret_cast<const NrrWord32*>(&t);
    for (uint32_t j = 0; j < 3; j++) {
        k[4 + j] = sc[j];
    }
    k[7] = mRdp.blend_color.a | (mRdp.grayscale ? 0x100u : 0u) | (fill ? 0x200u : 0u) |
           (static_cast<uint32_t>(tile) << 12);
    for (uint32_t j = 0; j < kKeyWords - 8; j++) {
        k[8 + j] = (fill || j >= 9) ? 0u : tw[j];
    }
    if (fill) {
        return true; /* a FILLRECT whose combiner samples a texture is a stock key (capture refuses) */
    }
    for (uint32_t s = 0; s < kSlots; s++) {
        const NrrHud::Slot& sl = N.slots[s];
        if (sl.hi != sl.lo && sl.lo == t.tmem_index) {
            for (uint32_t j = 0; j < 4; j++) {
                k[17 + j] = sl.src[j];
            }
        }
    }
    if (t.fmt == IM_FMT_CI) {
        k[21] = N.tlut[0];
        k[22] = N.tlut[1];
        return k[18] != 0 && k[21] != 0 && k[22] != 0;
    }
    return k[18] != 0;
}

GDX_COLD uint32_t Interp::NrrKeyHash(const NrrKey& k) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < kKeyWords; i++) {
        h = (h ^ k.w[i]) * 16777619u;
    }
    /* the memo indexes the LOW bits, which a multiply never feeds from higher ones: without a
     * final mix, keys that differ only above bit 7 -- font glyphs 0x100 bytes apart in one
     * sheet -- share one probe chain, and the 9th glyph cleared the whole memo (n60hud3) */
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h;
}

GDX_COLD Interp::NrrBlock* Interp::NrrDecFind(const NrrKey& k, uint32_t* hashOut) {
    const uint32_t h = NrrKeyHash(k);
    *hashOut = h;
    const uint32_t probe = mNiMenu != 0 ? kProbeMenu : kProbe;
    for (uint32_t p = 0; p < probe; p++) {
        NrrBlock& b = mNrr->blocks[(h + p) & (kBlocks - 1u)];
        if (b.used == 0) {
            return nullptr;
        }
        if (memcmp(&b.key, &k, sizeof(NrrKey)) == 0) {
            return &b;
        }
    }
    return nullptr;
}

/* The block of the rectangle about to draw from `tile` (`fill`: a FILLRECT), or nullptr when the
 * rectangle went the stock way (drawn already, its block captured when that is possible). A
 * colour / prim-depth / geometry word since the last rectangle of the same tile keeps its block. */
GDX_COLD Interp::NrrBlock* Interp::NrrDecBlockFor(uint8_t tile, bool fill, const Cmd& c, const GdxHudWord* t) {
    NrrHud& N = *mNrr;
    const uint8_t who = fill ? 0x80u : tile;
    if (!N.dirty && N.last != nullptr && N.lastWho == who && N.clock != -2) {
        return N.last;
    }
    NrrKey k;
    uint32_t h = 0;
    const bool known = NrrDecKey(k, tile, fill);
    NrrBlock* b = known ? NrrDecFind(k, &h) : nullptr;
    if ((mNrrLate & kLateScissor) != 0 && known && (b == nullptr || b->stock != 0)) {
        NrrBlock* base = NrrScSibling(k);
        NrrBlock* v = base != nullptr ? NrrScVariantOf(base, k, h) : nullptr;
        if (v != nullptr) {
            b = v; /* a new scissor of a captured block: no stock rectangle, no memo entry */
        }
    }
    b = NrrBlockMiss(known ? &k : nullptr, h, b, c, t, fill);
    if (b == nullptr) {
        return nullptr;
    }
    N.last = b;
    N.lastWho = who;
    N.dirty = false;
    return b;
}

/* ---- cold: the stock arms, the capture, materialisation ------------------------------------ */

/* A run off the hot loop: [native] nrr_hud=2 (verify), or nrr_hud_clock: the run with a clock pair
 * around it (>= 1), closed at its end (-1), every rectangle the stock way (-2; no programs). */
GDX_COLD void Interp::NrrRunCold(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    if (mNrrHud == 2) {
        NrrVerifyRun(w, n);
        return;
    }
    const uint64_t t0 = static_cast<uint64_t>(gdx_ninterp_now());
    NrrHudHotBody(w, n);
    if (N.clock == -1) {
        NrrHudHotClose();
    }
    N.w.ticks += static_cast<uint64_t>(gdx_ninterp_now()) - t0;
}

/* What a run's begin re-checks only when something moved: the atlas (a view a block names may
 * have moved: the memo goes), the task (its frame config: the compiled register words' stores
 * were computed under it -- NrrRegEnv), and the extra geometry mode (the corner mapping). */
GDX_COLD void Interp::NrrRunEpochs() {
    NrrHud& N = *mNrr;
    const unsigned long atlas = *N.atlasPtr;
    if (N.atlasEpoch != atlas) {
        if (GDX_UNLIKELY(mNiVolDiag > 0)) {
            mNiVolDiag--;
            GDXN_LOG("[nivol] memoclear why=atlas epoch=%lu->%lu seq=%u", N.atlasEpoch, atlas, N.seq);
        }
        NrrMemoClear(); /* an atlas view a block names may have moved (a retired slot: NrrBlockMiss) */
        N.atlasEpoch = atlas;
    }
    if (N.envSeq != N.seq) {
        uint32_t env[8];
        NrrRegEnv(env);
        if (memcmp(env, N.env, sizeof(env)) != 0) {
            if (GDX_UNLIKELY(mNiVolDiag > 0)) {
                mNiVolDiag--;
                GDXN_LOG("[nivol] progclear why=env seq=%u", N.seq);
            }
            memcpy(N.env, env, sizeof(env));
            NrrRegClear();
            NrrProgClear(); /* their ST ops carry the old config's stores */
        }
        N.envSeq = N.seq;
    }
    N.envEgm = mRsp.extra_geometry_mode;
    NrrXMode();
}

/* The run's word `i` through the walker's own handler (Exec). Returns the next word's index. */
GDX_COLD uint32_t Interp::NrrExecWord(const Cmd& c, uint32_t i) {
    NrrHud& N = *mNrr;
    N.frame.list.base = reinterpret_cast<const uint8_t*>(N.runWords);
    N.frame.list.limit = N.runN;
    N.frame.list.stride = static_cast<uint8_t>(sizeof(GdxHudWord));
    N.frame.list.isBig = false;
    N.frame.list.isF3d = false;
    N.frame.idx = i + 1;
    (void)Exec(c, N.frame, 0);
    return static_cast<uint32_t>(N.frame.idx);
}

/* No room in the core: the hand-off was refused (no open mono main-target frame: the 3D slider, a
 * render target) -- the rectangle is drawn the stock way and counted as a fallback -- or the
 * frame's pool is out, where the stock path would drop it too (counted by the core). */
GDX_COLD void Interp::NrrNoRing(const Cmd& c, const GdxHudWord* t, bool fill) {
    if (!mNrr->open) {
        NrrStockRect(c, t, fill, nullptr, 0);
    }
}

/* The words the decoder does not model: a TLUT load for real (its identity noted), extra geometry
 * (the corner mapping follows), triangles, a malformed TEXRECT and a depth-clear FILLRECT (the
 * interpreter draws: citro3d handed back, TMEM settled first). Returns the next index. */
GDX_COLD uint32_t Interp::NrrColdWord(const Cmd& c, uint32_t i) {
    NrrHud& N = *mNrr;
    const uint8_t op = Opcode(c.w0);
    N.dirty = true;
    if (op == OP_LOADTLUT) {
        N.w.rloads++;
        (void)NrrExecWord(c, i);
        NrrNoteTlut(c);
        return i + 1;
    }
    if (op == OP_EXTRAGEOMETRYMODE) {
        (void)NrrExecWord(c, i);
        N.envEgm = mRsp.extra_geometry_mode;
        NrrXMode();
        return i + 1;
    }
    NrrHudHotClose(); /* the interpreter draws next: citro3d back to the backend */
    NrrMaterializeAll();
    const uint32_t next = NrrExecWord(c, i);
    N.last = nullptr;
    N.drew = true;
    N.w.interp++;
    N.sesFallback++;
    return next;
}

/* ---- compiled register words --------------------------------------------------------------- */

/* What DpSetScissor's mapping reads besides its word (AdjustViewportOrScissor, the border trim):
 * a compiled SETSCISSOR is valid while these hold (NrrRunEpochs compares them once per task). */
GDX_COLD void Interp::NrrRegEnv(uint32_t env[8]) const {
    env[0] = mCfg.nativeW;
    env[1] = mCfg.nativeH;
    env[2] = mCfg.curW;
    env[3] = mCfg.curH;
    env[4] = mCfg.windowH;
    env[5] = mCfg.rendersToFb;
    env[6] = mCfg.removeBorders ? 1u : 0u;
    env[7] = mBackend->GetClipParameters().invertY ? 1u : 0u;
}

GDX_COLD void Interp::NrrRegClear() {
    NrrHud& N = *mNrr;
    for (uint32_t k = 0; k < kRegFx; k++) {
        N.regs[k].used = 0;
    }
    N.nRegs = 0;
}

/* The register memory a compiled word may write: SETTIMG's operand, the tiles .. the scissor, and
 * the RSP's geometry / fog / extra geometry / texture-scale block. Word-aligned spans. */
GDX_COLD int Interp::NrrRegRegions(uint8_t* base[3], uint32_t len[3]) {
    base[0] = reinterpret_cast<uint8_t*>(&mRdp.texture_to_load);
    len[0] = sizeof(mRdp.texture_to_load);
    base[1] = reinterpret_cast<uint8_t*>(&mRdp.texture_tile[0]);
    len[1] = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(&mRdp.z_buf_address) - base[1]);
    base[2] = reinterpret_cast<uint8_t*>(&mRsp.geometry_mode);
    len[2] = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(&mRsp.texture_scaling_factor + 1) - base[2]);
    for (int r = 0; r < 3; r++) {
        if ((reinterpret_cast<uintptr_t>(base[r]) & 3u) != 0) {
            return 0;
        }
        len[r] &= ~3u;
    }
    return len[0] + len[1] + len[2] <= kFxScratch ? 3 : 0;
}

/* The register-region bytes the compile must not capture as stores: textures_changed (written
 * only on a value change: the `arm` flag reproduces it). */
GDX_COLD uint32_t Interp::NrrRegKeepMask(const uint8_t* word) const {
    uint32_t keep = 0;
    const uint8_t* tc = reinterpret_cast<const uint8_t*>(&mRdp.textures_changed[0]);
    for (uint32_t b = 0; b < 4; b++) {
        if (word + b == tc || word + b == tc + 1) {
            keep |= 0xFFu << (8u * b);
        }
    }
    return keep;
}

static void NrrRegCopy(uint8_t* const base[3], const uint32_t len[3], uint8_t* buf, bool toRegs, bool invert) {
    uint32_t at = 0;
    for (int r = 0; r < 3; r++) {
        for (uint32_t b = 0; b < len[r]; b++) {
            if (toRegs) {
                base[r][b] = invert ? static_cast<uint8_t>(~buf[at + b]) : buf[at + b];
            } else {
                buf[at + b] = base[r][b];
            }
        }
        at += len[r];
    }
}

/* A word the hot path could not take: compile it when it can be (its handler runs for real while
 * it is compiled), else run its handler. */
GDX_COLD uint32_t Interp::NrrRegCold(const Cmd& c, uint32_t i, uint32_t h, const NrrRegFx** fxOut) {
    NrrHud& N = *mNrr;
    *fxOut = nullptr;
    const uint8_t op = Opcode(c.w0);
    if (op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD || op == OP_LOADTLUT || op == OP_EXTRAGEOMETRYMODE) {
        return NrrColdWord(c, i);
    }
    N.w.regCold++;
    N.dirty = N.dirty || NrrRegDirties(op);
    if (!NrrRegCompilable(op) || mBufNumTris != 0 || mTexOv != nullptr) {
        return NrrExecWord(c, i);
    }
    NrrRegFx* slot = nullptr;
    for (uint32_t p = 0; p < kRegProbe && slot == nullptr; p++) {
        NrrRegFx& fx = N.regs[(h + p) & (kRegFx - 1u)];
        if (fx.used == 0) {
            slot = &fx;
        } else if (fx.w0 == c.w0 && fx.w1 == c.w1 && fx.w1hi == c.w1hi) {
            if (fx.n != kFxSeen) {
                return NrrExecWord(c, i); /* an exec-only word */
            }
            slot = &fx; /* its second sight: compile it */
        }
    }
    if (slot == nullptr || N.nRegs + 1u >= kRegFx) {
        NrrRegClear(); /* full: start over */
        slot = &N.regs[h & (kRegFx - 1u)];
    }
    if (slot->used == 0) {
        N.nRegs++;
        slot->w0 = c.w0;
        slot->w1 = c.w1;
        slot->w1hi = c.w1hi;
        slot->used = 1;
        slot->n = kFxSeen; /* a word seen once runs its handler (one-off colours do not pay a compile) */
        return NrrExecWord(c, i);
    }
    NrrRegCompile(*slot, c, i);
    if (slot->n <= kFxStores) {
        *fxOut = slot;
    }
    return i + 1;
}

/* Compile a register word: its handler runs for real on the live registers (S -> R), then once
 * more on the bitwise complement of S (-> P), and the live registers are put back to R. A bit the
 * handler writes is the same in R and P (its value is a function of the word); a bit it does not
 * write is S in R and ~S in P. So mask = ~(R ^ P), value = R & mask -- checked: every unwritten
 * bit must have kept its value in both runs, else the word stays exec-only. The handler's
 * conditional effects are not stores: the texture arm (SETTILE / SETTILESIZE) and the empty-flush
 * counter (prim / env / prim depth) are flags applied when the stores change something. */
GDX_COLD void Interp::NrrRegCompile(NrrRegFx& fx, const Cmd& c, uint32_t i) {
    NrrHud& N = *mNrr;
    const uint8_t op = Opcode(c.w0);
    memset(static_cast<void*>(&fx), 0, sizeof(fx));
    fx.w0 = c.w0;
    fx.w1 = c.w1;
    fx.w1hi = c.w1hi;
    fx.used = 1;
    fx.n = kFxExecOnly;
    fx.arm = (op == OP_SETTILE || op == OP_SETTILESIZE) ? 1 : 0;
    fx.why = op == OP_SETPRIMCOLOR ? FLUSH_PRIM : op == OP_SETENVCOLOR ? FLUSH_ENV
             : op == OP_SETPRIMDEPTH                                    ? FLUSH_PRIMDEPTH
                                                                        : FLUSH_NONE;
    fx.dirty = NrrRegDirties(op) ? 1 : 0;
    N.w.regBuilds++;
    uint8_t* base[3];
    uint32_t len[3];
    if (NrrRegRegions(base, len) == 0) {
        (void)NrrExecWord(c, i);
        return;
    }
    uint32_t flushSaved[FLUSH_COUNT];
    NrrRegCopy(base, len, N.fxS, false, false);
    (void)NrrExecWord(c, i); /* the real run */
    NrrRegCopy(base, len, N.fxR, false, false);
    memcpy(flushSaved, mStats.flush, sizeof(flushSaved));
    NrrRegCopy(base, len, N.fxS, true, true); /* the complement */
    (void)NrrExecWord(c, i);
    NrrRegCopy(base, len, N.fxP, false, false);
    NrrRegCopy(base, len, N.fxR, true, false); /* the real result back */
    memcpy(mStats.flush, flushSaved, sizeof(flushSaved));
    uint32_t at = 0, n = 0;
    bool ok = true;
    for (int r = 0; r < 3 && ok; r++) {
        for (uint32_t o = 0; o < len[r] && ok; o += 4) {
            uint32_t s, rv, pv;
            memcpy(&s, N.fxS + at + o, 4);
            memcpy(&rv, N.fxR + at + o, 4);
            memcpy(&pv, N.fxP + at + o, 4);
            const uint32_t keep = NrrRegKeepMask(base[r] + o);
            const uint32_t mask = ~(rv ^ pv) & ~keep;
            if (((rv ^ s) & ~mask & ~keep) != 0 || ((pv ^ ~s) & ~mask & ~keep) != 0) {
                ok = false; /* an unwritten bit moved: not a pure masked write */
            } else if (mask != 0) {
                if (n == kFxStores) {
                    ok = false;
                } else {
                    fx.dst[n] = reinterpret_cast<NrrWord32*>(base[r] + o);
                    fx.mask[n] = mask;
                    fx.val[n] = rv & mask;
                    n++;
                }
            }
        }
        at += len[r];
    }
    fx.n = ok ? static_cast<uint8_t>(n) : kFxExecOnly;
}

/* [native] nrr_hud_late bit 4 (n60intro): the open interpreter batch, drawn (painter's order: it
 * precedes every rectangle of the run, as it does when the stock path's next state change flushes it). */
GDX_COLD void Interp::NrrLateFlush() {
    Flush(FLUSH_HOOK);
    mNrr->w.lateFlushes++;
}

/* [native] nrr_hud_late bit 2 (n60intro): a wide LD op's line pitch -- DpLoadBlock's decision under the
 * tiles of the moment (LoadBlockLineBytes), recorded in its SETTIMG word -- is the one the live tiles
 * give. A program then needs none of the other tiles as entry dependencies. */
GDX_COLD bool Interp::NrrLdPitchOk(const uint32_t* r, const Cmd& c) {
    const uint8_t loadTile = static_cast<uint8_t>(C1(c.w1, 24, 3));
    const uint32_t lo = r[4] & 0xFFFFu;
    const uint32_t bytes = NrrTexelBytes(C1(c.w1, 12, 12) + 1u, mRdp.texture_to_load.siz);
    if (LoadBlockLineBytes(loadTile, static_cast<uint16_t>(lo), bytes, C1(c.w1, 0, 12)) == NrrWideLine(r[0])) {
        return true;
    }
    mNrr->w.pitchMiss++;
    return false;
}

/* [native] nrr_hud_late bit 1 (n60intro): a program rectangle under another scissor than its block's.
 * The block of the live key (the block's key, its scissor words the live ones): the memo's own, else
 * a variant of `base` -- a copy whose only change is the scissor and the core state resolved for it
 * (the scissor reaches nothing else of a block: rectangles are clipped by the GPU, not the CPU) --
 * else the stock rectangle (its block captured under the live key). Variants live outside the memo:
 * the race intro's sliding boxes name a new scissor every frame, which would fill the memo. */
GDX_COLD Interp::NrrBlock* Interp::NrrScVariant(NrrBlock* base, const Cmd& c, const GdxHudWord* t, bool fill) {
    NrrHud& N = *mNrr;
    NrrKey k = base->key;
    const NrrWord32* sc = reinterpret_cast<const NrrWord32*>(&mRdp.scissor);
    for (uint32_t j = 0; j < 3; j++) {
        k.w[4 + j] = sc[j];
    }
    uint32_t h = 0;
    NrrBlock* found = NrrDecFind(k, &h);
    if (found != nullptr && found->stock == 0) {
        N.w.scVarTable++;
        return found;
    }
    /* (a stock key under this scissor: a degenerate one the core's key cannot carry -- the base's
     * capture holds everything else) */
    NrrBlock* v = NrrScVariantOf(base, k, h);
    return v != nullptr ? v : NrrBlockMiss(&k, h, nullptr, c, t, fill);
}

/* The variant of `base` under key `k` (base's key, the live scissor; `h` its hash), or nullptr. */
GDX_COLD Interp::NrrBlock* Interp::NrrScVariantOf(NrrBlock* base, const NrrKey& k, uint32_t h) {
    NrrHud& N = *mNrr;
    const uint32_t set = ((h >> 8) & (kScVars / 2u - 1u)) * 2u;
    uint32_t way = 0;
    for (; way < 2; way++) {
        const NrrHud::ScVar& w = N.scv[set + way];
        if (w.base == base && w.baseVer == base->ver && memcmp(&w.blk.key, &k, sizeof(NrrKey)) == 0) {
            break;
        }
    }
    if (way == 2) {
        way = N.scv[set].seq <= N.scv[set + 1].seq ? 0u : 1u; /* the older one goes */
    }
    NrrHud::ScVar& v = N.scv[set + way];
    if (v.base == base && v.baseVer == base->ver && base->used != 0 && v.blk.stGen == NrrStateGeneration() &&
        memcmp(&v.blk.key, &k, sizeof(NrrKey)) == 0) {
        N.w.scVarHits++;
        if (v.seq != N.seq) {
            NrrBlock* p = NrrScPromote(static_cast<uint32_t>(&v - N.scv), h);
            if (p != nullptr) {
                return p;
            }
        }
        return &v.blk;
    }
    if (base->used == 0 || base->stock != 0 ||
        (base->stGen != NrrStateGeneration() && !NrrResolveState(*base)) || base->st == nullptr) {
        return nullptr;
    }
    void* mem = NrrScStateMem(static_cast<uint32_t>(&v - N.scv));
    const Rect& r = mRdp.scissor;
    const NrrState* st = mem != nullptr ? NrrStateRescissor(base->st, mem, r.x, r.y, static_cast<int>(r.width),
                                                             static_cast<int>(r.height))
                                        : nullptr;
    if (st == nullptr) {
        v.base = nullptr;
        return nullptr;
    }
    v.blk = *base;
    v.blk.key = k;
    v.blk.scissor = r; /* == mRs.scissor after the stock rectangle's scissor update */
    v.blk.st = st;
    v.blk.stGen = base->stGen;
    v.blk.batchCls = kClasses + (N.nUnique++ & 0x0FFFFFFFu); /* batches only with itself */
    v.base = base;
    v.baseVer = base->ver;
    v.seq = N.seq;
    N.w.scVarBuilds++;
    N.memoEpoch++; /* [native] nrr_hud_memo: this address may have held another variant */
    return &v.blk;
}

/* The key-without-scissor index: its slot. */
static uint32_t NrrScSibSlot(const uint32_t* kw) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < kKeyWords; i++) {
        h = (h ^ (i >= 4 && i <= 6 ? 0u : kw[i])) * 16777619u;
    }
    h ^= h >> 15;
    return (h * 0x2C1B3C6Du >> 24) & (kScSibs - 1u);
}

/* A variant used again in a later task (a scissor that stays: the intro's boxes at rest) becomes a
 * memo block -- the same fields, its state resolved from its key the way a capture's is -- while the
 * memo has room to spare; a degenerate scissor (the core's key cannot carry it) stays a variant. */
GDX_COLD Interp::NrrBlock* Interp::NrrScPromote(uint32_t var, uint32_t h) {
    NrrHud& N = *mNrr;
    NrrHud::ScVar& v = N.scv[var];
    if (v.blk.scissor.width > 0x7FFFu || v.blk.scissor.height > 0x7FFFu || N.nBlocks + 1u >= (kBlocks * 3u) / 4u) {
        return nullptr;
    }
    NrrBlock* slot = nullptr;
    const uint32_t probe = mNiMenu != 0 ? kProbeMenu : kProbe;
    for (uint32_t p = 0; p < probe && slot == nullptr; p++) {
        NrrBlock& b = N.blocks[(h + p) & (kBlocks - 1u)];
        if (b.used == 0) {
            slot = &b;
        }
    }
    if (slot == nullptr) {
        return nullptr;
    }
    *slot = v.blk;
    slot->used = 1;
    slot->ver = ++N.blockVer;
    slot->stNoSc = nullptr;
    if (!NrrResolveState(*slot)) {
        slot->used = 0;
        return nullptr;
    }
    N.nBlocks++;
    N.scSib[NrrScSibSlot(slot->key.w)] = slot;
    N.w.scVarPromotes++;
    N.memoEpoch++;
    return slot;
}

/* A captured block whose key is `k` but for the scissor, or nullptr. */
GDX_COLD Interp::NrrBlock* Interp::NrrScSibling(const NrrKey& k) {
    NrrBlock* b = mNrr->scSib[NrrScSibSlot(k.w)];
    if (b == nullptr || b->used == 0 || b->stock != 0 || memcmp(b->key.w, k.w, 4 * sizeof(uint32_t)) != 0 ||
        memcmp(b->key.w + 7, k.w + 7, (kKeyWords - 7) * sizeof(uint32_t)) != 0) {
        return nullptr;
    }
    return b;
}

/* Is `b` a scissor variant (not a memo block)? Then its base. */
GDX_COLD Interp::NrrBlock* Interp::NrrScBaseOf(NrrBlock* b) {
    NrrHud& N = *mNrr;
    if (N.scv == nullptr) {
        return b;
    }
    const uintptr_t a = reinterpret_cast<uintptr_t>(b);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(&N.scv[0]);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(&N.scv[kScVars]);
    if (a < lo || a >= hi) {
        return b;
    }
    const NrrHud::ScVar& v = N.scv[(a - lo) / sizeof(NrrHud::ScVar)];
    return const_cast<NrrBlock*>(v.base);
}

/* The next core-state buffer of the variant ring for variant `owner`: never the state the open batch
 * or the hand-off last applied (a pointer compare decides whether a state is applied again), and
 * the variant that held it before forgets it. */
GDX_COLD void* Interp::NrrScStateMem(uint32_t owner) {
    NrrHud& N = *mNrr;
    for (uint32_t tries = 0; tries < kScStates; tries++) {
        const uint32_t i = N.scStateNext;
        N.scStateNext = (N.scStateNext + 1u) % kScStates;
        if (N.scState[i] == nullptr) {
            N.scState[i] = ::operator new(NrrStateBytes(), std::nothrow);
            if (N.scState[i] == nullptr) {
                return nullptr;
            }
        }
        if (N.scState[i] == static_cast<const void*>(N.batchSt) || N.scState[i] == static_cast<const void*>(N.ctx.last)) {
            continue;
        }
        if (N.scStateOwner[i] != 0) {
            NrrHud::ScVar& old = N.scv[N.scStateOwner[i] - 1u];
            if (old.blk.st == N.scState[i]) {
                old.base = nullptr;
            }
        }
        N.scStateOwner[i] = static_cast<uint8_t>(owner + 1u);
        return N.scState[i];
    }
    return nullptr;
}

/* A rectangle whose block the hot path could not use as it was (no identity, no block, a stale or
 * stock block, the bisect lever): settle it -- the stock rectangle (captured when its key is
 * known and it has no block yet, or a stale one), a re-resolved state block, or a source
 * re-check. Returns the block to draw from, or nullptr when the stock rectangle drew. */
GDX_COLD Interp::NrrBlock* Interp::NrrBlockMiss(const NrrKey* key, uint32_t h, NrrBlock* b, const Cmd& c,
                                                 const GdxHudWord* t, bool fill) {
    NrrKey kc; /* the key may live in `b`, which a recapture rewrites */
    if (key != nullptr) {
        kc = *key;
    }
    const NrrKey* k = key != nullptr ? &kc : nullptr;
    if (mNrr->clock == -2 || k == nullptr) {
        NrrStockRect(c, t, fill, nullptr, 0); /* -2: every rectangle the stock way (a bisect lever) */
        return nullptr;
    }
    if (b == nullptr || !NrrBlockFresh(*b)) {
        NrrStockRect(c, t, fill, k, h);
        return nullptr;
    }
    if ((b->stock != 0 || b->stGen != NrrStateGeneration()) && !NrrResolveState(*b)) {
        NrrStockRect(c, t, fill, nullptr, 0);
        return nullptr;
    }
    return b;
}

/* A block a hit may draw from: its texture source still holds the bytes it was captured with
 * (checked once per task; an immutable source never moves; UNKNOWN sources rely on the game's
 * own invalidation, which retires the cache slot). */
GDX_COLD bool Interp::NrrBlockFresh(NrrBlock& b) {
    if (!b.usesTex) {
        return true;
    }
    if (mNrrSlotGen[b.slot] != b.slotGen) {
        return false; /* the cache slot was retired since the capture: recapture */
    }
    if (b.imm != 0 || b.checkedSeq == mNrr->seq) {
        return true;
    }
    return NrrBlockRecheck(b);
}

/* The batch's TexEnv constants for an exotic block (an input reads a key / convert / lod-fraction
 * register): HudPackAux itself. */
GDX_COLD void Interp::NrrAuxCold(const NrrBlock& b, float out[7][4]) {
    Gdx3dsVboPackAux aux;
    HudPackAux(b.ds, &mRsp.loaded_vertices[kMaxVertices], &aux);
    memcpy(out, aux.inputRgba, sizeof(aux.inputRgba));
    mNrr->w.auxCold++;
}

/* The batch class of a state key: its index in the interned table (a key the full table cannot
 * hold gets an id of its own -- it then batches only with itself). */
GDX_COLD uint32_t Interp::NrrInternClass(const NrrStateKey& k) {
    NrrHud& N = *mNrr;
    for (uint32_t i = 0; i < N.nClasses; i++) {
        if (memcmp(&N.classes[i], &k, sizeof(NrrStateKey)) == 0) {
            return i;
        }
    }
    if (N.nClasses < kClasses) {
        N.classes[N.nClasses] = k;
        return N.nClasses++;
    }
    return kClasses + (N.nUnique++ & 0x0FFFFFFFu);
}

/* The NRR core takes citro3d (once per HUD region, kept across runs): the interpreter's open batch
 * drawn first (painter's order). false = the core refused (no open frame on the main target). */
GDX_COLD bool Interp::NrrHudOpen() {
    NrrHud& N = *mNrr;
    Flush(FLUSH_HOOK);
    N.simLive = mBackend->NrrScissorLive();
    if (gdx3ds_nrr_open(&N.ctx, nullptr, NRR_CAT_HUD) == 0) {
        return false;
    }
    N.simVp = mRs.viewport;
    N.simSc = mRs.scissor;
    N.simVpSet = N.simScSet = N.simLastVp = false;
    N.defaultVp.x = 0;
    N.defaultVp.y = static_cast<int16_t>(mCfg.nativeH);
    N.defaultVp.width = mCfg.nativeW;
    N.defaultVp.height = mCfg.nativeH;
    AdjustViewportOrScissor(&N.defaultVp);
    N.simVpDefault = NrrRectEq(N.simVp, N.defaultVp);
    N.open = true;
    mNrrOpen = true;
    N.batchSt = nullptr;
    N.batchBlock = nullptr;
    N.batchCls = kNoClass;
    N.batchVerts = 0;
    N.batchEnd = nullptr;
    return true;
}

/* After the hand-off: the backend and mRs where the stock HUD rectangles would have left them --
 * the default viewport, the last scissor, and the scissor on or off in the stock order. */
GDX_COLD void Interp::NrrStockViewportScissor() {
    NrrHud& N = *mNrr;
    if (N.simScSet && !NrrRectEq(mRs.scissor, N.simSc)) {
        mBackend->SetScissor(N.simSc.x, N.simSc.y, N.simSc.width, N.simSc.height);
        mRs.scissor = N.simSc;
    }
    if (N.simVpSet && (!NrrRectEq(mRs.viewport, N.simVp) || N.simLastVp)) {
        mBackend->SetViewport(N.simVp.x, N.simVp.y, N.simVp.width, N.simVp.height);
        mRs.viewport = N.simVp;
    }
    if (N.simLive && !N.simLastVp && N.simVpSet) {
        mBackend->SetScissor(N.simSc.x, N.simSc.y, N.simSc.width, N.simSc.height); /* on again, as the stock order */
    }
    N.simVpSet = N.simScSet = N.simLastVp = false;
}

/* The core hands citro3d back: the open batch drawn, the hand-off closed, the backend's viewport /
 * scissor where the stock rectangles would have left them. Once per HUD region. */
GDX_COLD void Interp::NrrHudClose() {
    NrrHud& N = *mNrr;
    mNrrOpen = false;
    NrrHudHotDraw();
    gdx3ds_nrr_close(&N.ctx);
    N.open = false;
    N.batchSt = nullptr;
    N.batchBlock = nullptr;
    N.batchCls = kNoClass;
    if (N.simVpSet || N.simScSet) {
        NrrStockViewportScissor();
    }
}

/* The first rectangle of a hand-off whose viewport is not the default one: the stock rectangle's
 * SetViewport(default), which turns citro3d's scissor off. */
GDX_COLD void Interp::NrrSimDefaultViewport() {
    NrrHud& N = *mNrr;
    N.simVp = N.defaultVp;
    N.simLive = false;
    N.simVpSet = true;
    N.simLastVp = true;
    N.simVpDefault = true;
}

/* The block's state with the scissor off (cold, once). */
GDX_COLD bool Interp::NrrResolveNoScissor(NrrBlock& b) {
    NrrStateKey k = b.batchKey;
    const int page = b.usesTex ? mBackend->TexPage(b.texId) : -1;
    k.tex[0] = NrrStateTex(b);
    k.scissor[0] = k.scissor[1] = k.scissor[2] = k.scissor[3] = 0; /* the core: all 0 = scissor disabled */
    b.stNoSc = NrrHudState(k);
    b.batchKeyNoSc = k;
    if (page >= 0) {
        b.batchKeyNoSc.tex[0] = NRR_TEX_PAGE | static_cast<uint32_t>(page);
    }
    b.batchClsNoSc = NrrInternClass(b.batchKeyNoSc);
    return b.stNoSc != nullptr;
}

/* A load the NRR does not virtualise: the stock handler (the interpreter-write hook settles any
 * pending load it only partly overwrites first, and drops the identities it covers). */
GDX_COLD void Interp::NrrLoadCold(const Cmd& c) {
    NrrHud& N = *mNrr;
    uint32_t keep = 0;
    for (uint32_t k = 0; k < N.nPend; k++) {
        if (N.pend[k].lo != N.pend[k].hi) {
            N.pend[keep++] = N.pend[k]; /* the dead ones out (the pending list was full) */
        }
    }
    N.nPend = keep;
    N.w.rloads++;
    DpLoadBlock(static_cast<uint8_t>(C1(c.w1, 24, 3)), C0(c.w0, 12, 12), C0(c.w0, 0, 12), C1(c.w1, 12, 12),
                C1(c.w1, 0, 12));
}

/* A real LOADTLUT just ran: which operand each palette half now holds (DpLoadTlut's split). */
GDX_COLD void Interp::NrrNoteTlut(const Cmd& c) {
    NrrHud& N = *mNrr;
    const auto& tl = mRdp.texture_to_load;
    const uint16_t tmem = mRdp.texture_tile[C1(c.w1, 24, 3)].tmem;
    const uint32_t id = tl.cmd.w1hi == kHostTag ? (tl.cmd.w1 != 0 ? tl.cmd.w1 : 1u) : 0u;
    const uint32_t count = C1(c.w1, 14, 10) + 1u;
    if (tmem >= 256) {
        const uint32_t off = (tmem - 256u) * 2u;
        if (count == 256u && off == 0) {
            N.tlut[0] = id;
            N.tlut[1] = id;
        } else if (off < 256u) {
            N.tlut[0] = id;
        } else {
            N.tlut[1] = id;
        }
    } else {
        N.tlut[1] = id;
    }
}

/* The per-task source check of a block whose texture is not immutable (NrrHudHotFresh). */
GDX_COLD bool Interp::NrrBlockRecheck(NrrBlock& b) {
    if (b.cls == GDX_TMEMSRC_UNKNOWN) {
        b.checkedSeq = mNrr->seq;
        return true;
    }
    uint64_t now[2];
    gdx_gfx_tmem_source_stamp(now);
    if (gdx_gfx_tmem_source_unchanged(reinterpret_cast<uintptr_t>(b.srcAddr), b.srcBytes, b.cls, b.stamp) == 0) {
        return false;
    }
    b.stamp[0] = now[0];
    b.stamp[1] = now[1];
    b.checkedSeq = mNrr->seq;
    return true;
}

/* == HudRectPositions' x, every branch (a widescreen-UI extra geometry mode). */
GDX_COLD void Interp::NrrRectXFull(int32_t ulx, int32_t lrx, float* x0, float* x1) const {
    const float halfW = static_cast<float>(mCfg.nativeW / 2);
    float ulxf = static_cast<float>(ulx);
    float lrxf = static_cast<float>(lrx);
    ulxf = ulxf / (4.0f * halfW) - 1.0f;
    lrxf = lrxf / (4.0f * halfW) - 1.0f;
    const float currentAspect = static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH);
    const uint32_t widescreenMode = mRsp.extra_geometry_mode & (0x04u | 0x08u | 0x10u | 0x20u);
    const bool widescreenFrameActive = currentAspect > (4.0f / 3.0f) && mCfg.widescreen && !mCfg.forceFixedAspect;
    const bool stretchActive = widescreenFrameActive && (widescreenMode & 0x04u) != 0;
    const bool enhancedWidescreenUi = widescreenFrameActive && widescreenMode != 0 && mCfg.widescreenUi;
    if (stretchActive) {
        constexpr float kSafeAreaScale = 160.0f / (160.0f - 12.0f);
        ulxf *= kSafeAreaScale;
        lrxf *= kSafeAreaScale;
    } else if (enhancedWidescreenUi && (widescreenMode & 0x20u) != 0) {
        const float aspectScale = (4.0f / 3.0f) / currentAspect;
        const float center = (ulxf + lrxf) * 0.5f;
        const float halfWidth = (lrxf - ulxf) * 0.5f * aspectScale;
        ulxf = center - halfWidth;
        lrxf = center + halfWidth;
    } else {
        ulxf = AdjXForAspectRatio(ulxf);
        lrxf = AdjXForAspectRatio(lrxf);
        if (enhancedWidescreenUi) {
            const bool anchorLeft = (widescreenMode & 0x08u) != 0;
            const bool anchorRight = (widescreenMode & 0x10u) != 0;
            if (anchorLeft != anchorRight) {
                const float uiAspect = currentAspect < mCfg.hudMaxAspect ? currentAspect : mCfg.hudMaxAspect;
                float offset = (anchorRight ? 1.0f : -1.0f) * ((uiAspect - (4.0f / 3.0f)) / currentAspect);
                if (mCfg.removeBorders) {
                    offset += (anchorRight ? 1.0f : -1.0f) * (0.1f / currentAspect);
                }
                ulxf += offset;
                lrxf += offset;
            }
        }
    }
    *x0 = ulxf;
    *x1 = lrxf;
}

/* The corner mapping's constants for this task's frame config and the live extra geometry mode. */
GDX_COLD void Interp::NrrXMode() {
    NrrHud& N = *mNrr;
    const float before[7] = { N.aspect, N.x4HalfW, N.y4HalfH, N.xMul, N.xyDiv[0], N.xFull ? 1.0f : 0.0f,
                              N.xAdj ? 1.0f : 0.0f };
    const float currentAspect = static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH);
    const uint32_t widescreenMode = mRsp.extra_geometry_mode & (0x04u | 0x08u | 0x10u | 0x20u);
    const bool widescreenFrameActive = currentAspect > (4.0f / 3.0f) && mCfg.widescreen && !mCfg.forceFixedAspect;
    const bool stretchActive = widescreenFrameActive && (widescreenMode & 0x04u) != 0;
    const bool enhancedWidescreenUi = widescreenFrameActive && widescreenMode != 0 && mCfg.widescreenUi;
    N.xFull = stretchActive || enhancedWidescreenUi;
    N.xAdj = mCfg.widescreen && !mCfg.forceFixedAspect;
    N.aspect = currentAspect;
    N.x4HalfW = 4.0f * static_cast<float>(mCfg.nativeW / 2);
    N.y4HalfH = 4.0f * static_cast<float>(mCfg.nativeH / 2);
    N.xMul = N.xAdj ? 4.0f / 3.0f : 1.0f; /* x * (4/3) / aspect, or x * 1 / 1 == x exactly */
    N.xyDiv[0] = N.xAdj ? currentAspect : 1.0f;
    N.xyDiv[1] = 1.0f;
    N.xyHalf[0] = N.x4HalfW;
    N.xyHalf[1] = N.y4HalfH;
    const float after[7] = { N.aspect, N.x4HalfW, N.y4HalfH, N.xMul, N.xyDiv[0], N.xFull ? 1.0f : 0.0f,
                             N.xAdj ? 1.0f : 0.0f };
    if (memcmp(before, after, sizeof(before)) != 0) {
        N.memoEpoch++; /* [native] nrr_hud_memo: the corner mapping moved */
    }
}

/* == DpFillRectangle's full-screen widening. e = {ulx, uly, lrx, lry}. */
GDX_COLD void Interp::NrrFillFullScreen(int32_t e[4]) const {
    const bool isFullScreen = (e[2] == (static_cast<int32_t>(mCfg.nativeW - 1) * 4) &&
                               e[3] == (static_cast<int32_t>(mCfg.nativeH - 1) * 4));
    if (!isFullScreen) {
        return;
    }
    const float fsAspect = static_cast<float>(mCfg.curW) / static_cast<float>(mCfg.curH);
    const float fsK = (4.0f / 3.0f) / (fsAspect > 4.0f / 3.0f ? fsAspect : 4.0f / 3.0f);
    int32_t ex = static_cast<int32_t>(160.0f * (1.0f / (fsK > 0.05f ? fsK : 0.05f) + 1.0f) * 4.0f) + 64;
    if (ex < 2048) {
        ex = 2048;
    }
    e[0] = -ex;
    e[1] = -1024;
    e[2] = ex;
    e[3] = 2048;
}

/* NrrHudHotCorners for a block whose vertex colour / alpha needs the full gdxInput switch, or under
 * a widescreen-UI extra geometry mode (x: every branch of HudRectPositions, NrrRectXFull). */
GDX_COLD void Interp::NrrCornersCold(const NrrBlock& b, const int32_t e[4], const float st[4], bool flip,
                                      float* out) const {
    NrrHud& N = *mNrr;
    const bool cold = b.coldColour != 0;
    NrrHudHotCornersAt(b, e, st, flip, cold ? &kRgbaOne : b.colSrc, cold ? &kRgbaOne : b.alphaSrc, out);
    if (N.xFull) {
        float x[2];
        NrrRectXFull(e[0], e[2], &x[0], &x[1]);
        static const uint8_t kSlotX[6] = { 0, 0, 1, 0, 1, 1 }; /* slots ul ll ur ll lr ur */
        for (int s = 0; s < 6; s++) {
            out[s * kStride] = x[kSlotX[s]];
        }
    }
    if (b.coldColour == 0) {
        return;
    }
    const LoadedVertex* lv = &mRsp.loaded_vertices[kMaxVertices];
    const int ci = b.params.vtxColorInput - 1;
    const int ai = b.params.vtxAlphaInput - 1;
    const bool shadeAlphaIsOne = b.ds.use_fog && !b.ds.use_blend_color;
    for (int i = 0; i < 4; i++) {
        float* o = out + kCornerSlot[i] * kStride;
        if (ci >= 0) {
            const Rgba cc = b.ds.comb->shader_input_mapping[0][ci] == CCMUX_SHADE
                                ? lv[i].color
                                : NrrInputGeneric(mRdp, b.ds.comb, ci, 0, &lv[i], &lv[0]).c;
            o[6] = mByteToUnit[cc.r];
            o[7] = mByteToUnit[cc.g];
            o[8] = mByteToUnit[cc.b];
        }
        if (b.params.fogFactorToAlpha) {
            o[9] = mByteToUnit[lv[i].color.a];
        } else if (b.params.wantVtxAlpha) {
            if (ai >= 0 && b.ds.comb->shader_input_mapping[1][ai] == CCMUX_SHADE) {
                o[9] = shadeAlphaIsOne ? 1.0f : mByteToUnit[lv[i].color.a];
            } else {
                const NrrIn av = NrrInputGeneric(mRdp, b.ds.comb, ai, 1, &lv[i], &lv[0]);
                o[9] = (shadeAlphaIsOne && av.isShade) ? 1.0f : mByteToUnit[av.c.a];
            }
        }
    }
    memcpy(out + 3 * kStride, out + 1 * kStride, kStride * sizeof(float));
    memcpy(out + 5 * kStride, out + 2 * kStride, kStride * sizeof(float));
}

uint8_t NrrConstRegs(Backend* backend, const Combiner* comb, bool* exotic);

/* GdxInput's switch, resolved to a byte of the selector table (NrrHudHotAux) for input j's channel
 * c (0..2 colour from k = 0, 3 alpha from k = 1); `exotic`: it reads a key / convert / lod-fraction
 * register (the block then takes HudPackAux itself). */
GDX_COLD static uint8_t NrrAuxSource(const Combiner* comb, int j, int k, int c, bool* exotic) {
    enum : uint8_t { kPrim = 0, kEnv = 4, kShade = 8, kLod = 12, kZero = 13 };
    const uint8_t m = comb->shader_input_mapping[k][j];
    if (k == 1 && m == ACMUX_PRIM_LOD_FRAC) {
        return c == 3 ? kLod : kZero; /* tmp = {0, 0, 0, lod} */
    }
    switch (m) {
        case CCMUX_PRIMITIVE: return static_cast<uint8_t>(kPrim + c);
        case CCMUX_SHADE: return static_cast<uint8_t>(kShade + c);
        case CCMUX_SHADE_ALPHA: return kShade + 3;
        case CCMUX_ENVIRONMENT: return static_cast<uint8_t>(kEnv + c);
        case CCMUX_PRIMITIVE_ALPHA: return c == 3 ? kZero : kPrim + 3;
        case CCMUX_ENV_ALPHA: return c == 3 ? kZero : kEnv + 3;
        case CCMUX_PRIM_LOD_FRAC: return c == 3 ? kZero : kLod;
        case CCMUX_LOD_FRACTION:
        case CCMUX_KEY_CENTER:
        case CCMUX_KEY_SCALE:
        case CCMUX_CONVERT_K4:
        case CCMUX_CONVERT_K5: *exotic = true; return kZero;
        default: return kZero;
    }
}

/* The block's HudPackAux selectors: colour channels from k = 0, alpha from k = 1 under use_alpha
 * (else 1.0); fog makes it exotic (a fog block is a stock key anyway). */
GDX_COLD void Interp::NrrAuxSelect(NrrBlock& b, bool* exotic) {
    const DrawState& ds = b.ds;
    b.auxN = static_cast<uint8_t>(ds.numInputs < 7 ? ds.numInputs : 7);
    memset(b.auxSel, 0, sizeof(b.auxSel));
    for (int j = 0; j < b.auxN; j++) {
        for (int c = 0; c < 3; c++) {
            b.auxSel[j][c] = NrrAuxSource(ds.comb, j, 0, c, exotic);
        }
        b.auxSel[j][3] = ds.use_alpha ? NrrAuxSource(ds.comb, j, 1, 3, exotic) : 0xFFu;
    }
    if (ds.use_fog) {
        *exotic = true;
    }
}

/* A rectangle the NRR has no block for (or whose key is a stock key): TMEM settled, citro3d
 * handed back, the stock rectangle drawn, and -- when `k` is given -- its block captured. */
GDX_COLD void Interp::NrrStockRect(const Cmd& c, const GdxHudWord* t, bool fill, const NrrKey* k, uint32_t hash) {
    NrrHud& N = *mNrr;
    NrrHudHotClose();
    NrrMaterializeAll();
    const uint32_t emit0 = mStats.triEmitAll;
    const uint8_t tile = fill ? mRdp.first_tile_index : static_cast<uint8_t>(C1(c.w1, 24, 3));
    if (fill) {
        DpFillRectangle(static_cast<int32_t>(C1(c.w1, 12, 12)), static_cast<int32_t>(C1(c.w1, 0, 12)),
                        static_cast<int32_t>(C0(c.w0, 12, 12)), static_cast<int32_t>(C0(c.w0, 0, 12)));
    } else {
        DpTextureRectangle(static_cast<int32_t>(C1(c.w1, 12, 12)), static_cast<int32_t>(C1(c.w1, 0, 12)),
                           static_cast<int32_t>(C0(c.w0, 12, 12)), static_cast<int32_t>(C0(c.w0, 0, 12)), tile,
                           static_cast<int16_t>(t[1].w1 >> 16), static_cast<int16_t>(t[1].w1),
                           static_cast<int16_t>(t[2].w1 >> 16), static_cast<int16_t>(t[2].w1),
                           Opcode(c.w0) == OP_TEXRECTFLIP);
    }
    N.last = nullptr;
    N.dirty = true;
    N.drew = true;
    if (k == nullptr) {
        N.w.stockRects++;
        N.sesFallback++;
        return;
    }
    NrrCaptureAfter(*k, hash, tile, fill, emit0, c, t);
}

/* The block of the stock rectangle that just drew (`emit0`: the triangle count before it), under
 * key `k`: captured from what it bound, self-checked, resolved -- or marked a stock key. */
GDX_COLD void Interp::NrrCaptureAfter(const NrrKey& key, uint32_t hash, uint8_t tile, bool fill, uint32_t emit0,
                                      const Cmd& c, const GdxHudWord* t) {
    NrrHud& N = *mNrr;
    const NrrKey* k = &key;
    /* the slot: the key's own (a refresh) or the first free one on its probe path */
    NrrBlock* slot = nullptr;
    const uint32_t probe = mNiMenu != 0 ? kProbeMenu : kProbe;
    for (uint32_t p = 0; p < probe && slot == nullptr; p++) {
        NrrBlock& b = N.blocks[(hash + p) & (kBlocks - 1u)];
        if (b.used == 0 || memcmp(&b.key, k, sizeof(NrrKey)) == 0) {
            slot = &b;
        }
    }
    N.w.builds++;
    if (slot == nullptr || N.nBlocks + 1u >= kBlocks) {
        if (mNiMenuKeep && NrrMenuKeepRefuse()) {
            mNiSplitWin.keepMemo++;
            return; /* menus-s2b ni_menu_keep: the stock rectangle stands, every block stays */
        }
        if (GDX_UNLIKELY(mNiVolDiag > 0)) {
            mNiVolDiag--;
            GDXN_LOG("[nivol] memoclear why=%s blocks=%u seq=%u", slot == nullptr ? "probe" : "count", N.nBlocks, N.seq);
        }
        NrrMemoClear(); /* full: start over (rare; the next rectangle rebuilds) */
        return;
    }
    if (slot->used == 0) {
        N.nBlocks++;
    }
    memset(static_cast<void*>(slot), 0, sizeof(NrrBlock));
    slot->key = *k;
    slot->used = 1;
    slot->ver = ++N.blockVer;
    const bool emitted = mStats.triEmitAll - emit0 == 2u;
    int why = 0;
    if (!emitted) {
        why = 1;
    } else if (!NrrBlockCapture(*slot, tile, fill)) {
        why = 2;
    } else if (!NrrBlockSelfCheck(*slot, c, t, fill)) {
        why = 3;
    } else if (!NrrResolveState(*slot)) {
        why = 4;
    }
    if (why == 0 && (mNrrLate & kLateScissor) != 0) {
        N.scSib[NrrScSibSlot(k->w)] = slot;
    }
    if (why != 0) {
        slot->stock = 1;
        N.w.stockKeys++;
        if (GDX_UNLIKELY(mNrrDepDiag > 0)) {
            mNrrDepDiag--;
            GDXN_LOG("[nrrdep] stockkey why=%d emit=%lu sc=%d,%d %lu x %lu", why,
                     static_cast<unsigned long>(mStats.triEmitAll - emit0), mRdp.scissor.x, mRdp.scissor.y,
                     static_cast<unsigned long>(mRdp.scissor.width), static_cast<unsigned long>(mRdp.scissor.height));
        }
    }
}

/* The state the stock rectangle just resolved, as a block. false = not expressible. */
GDX_COLD bool Interp::NrrBlockCapture(NrrBlock& b, uint8_t tile, bool fill) {
    mNrr->memoEpoch++; /* [native] nrr_hud_memo: a block's corner inputs (uv, z, colour sources) change */
    if (!mTriMemoValid || !mPackBatch || mPackDst == nullptr || mBufNumTris < 2) {
        return false;
    }
    const DrawState& ds = mTriMemo;
    if (ds.comb == nullptr || ds.usedTextures[1] || ds.use_fog || mPackParams.fogFactorToAlpha ||
        (fill && ds.usedTextures[0])) {
        return false;
    }
    b.ds = ds;
    b.copy = (mRdp.other_mode_h & (3u << MDSFT_CYCLETYPE)) == CYC_COPY ? 1 : 0;
    b.depth = mRs.depth_test_and_mask;
    b.decal = mRs.decal_mode;
    b.alpha = mRs.alpha_blend;
    b.scissor = mRs.scissor;
    b.params = mPackParams;
    const float z0 = -1.0f, w0 = 1.0f;
    b.z = ds.clip.zIsFrom0To1 ? (z0 + w0) / 2.0f : z0;
    b.usesTex = ds.usedTextures[0] ? 1 : 0;
    b.ySign = ds.clip.invertY ? -1.0f : 1.0f;
    NrrSelectInputs(b);
    bool splitExotic = false;
    bool exotic = false;
    b.constRegs = NrrConstRegs(mBackend, ds.comb, &splitExotic);
    NrrAuxSelect(b, &exotic);
    b.auxExotic = exotic ? 1 : 0;
    if (!b.usesTex) {
        return true;
    }
    if (mRs.textures[0] < 0 || !mRs.sampler_valid[0]) {
        return false;
    }
    b.slot = mRs.textures[0];
    b.texId = mTex[b.slot].texture_id;
    b.slotGen = mNrrSlotGen[b.slot];
    b.smpLinear = mRs.sampler_linear[0] ? 1 : 0;
    b.smpCms = mRs.sampler_cms[0];
    b.smpCmt = mRs.sampler_cmt[0];
    /* HudPackVerts' unit-0 preparation, per axis (u, v) */
    const Tile& ut = mRdp.texture_tile[ds.effective_tile[0]];
    const int shift[2] = { ut.shifts, ut.shiftt };
    const float origin[2] = { ut.uls / 4.0f, ut.ult / 4.0f };
    const uint32_t dim[2] = { ds.tex_width[0], ds.tex_height[0] };
    const uint32_t dim2[2] = { ds.tex_width2[0], ds.tex_height2[0] };
    const float wrapClamp[2] = { mPackParams.wrapClampU[0], mPackParams.wrapClampV[0] };
    const float scale[2] = { mPackParams.uScale[0], mPackParams.vScale[0] };
    const float viewOff[2] = { mPackParams.uOff[0], mPackParams.vOff[0] };
    for (int a = 0; a < 2; a++) {
        const int sh = shift[a];
        b.uv.pre[a] = sh == 0 ? 1.0f : (sh <= 10 ? 1.0f / static_cast<float>(1 << sh) : static_cast<float>(1 << (16 - sh)));
        b.uv.off[a] = origin[a];
        b.uv.mul[a] = (dim[a] & (dim[a] - 1u)) == 0u ? 1 : 0;
        b.uv.normDiv[a] = static_cast<float>(dim[a]);
        b.uv.normMul[a] = b.uv.mul[a] ? 1.0f / static_cast<float>(dim[a]) : 1.0f;
        b.uv.scale[a] = scale[a];
        b.uv.viewOff[a] = viewOff[a];
        b.uv.clamp[a] = wrapClamp[a];
        if (mPackParams.shaderClampGate && (ds.tm & (1u << a)) != 0) {
            b.uv.clamp[a] = ((dim2[a] - 0.5f) / dim[a]) * scale[a]; /* the shader-clamp extent */
        }
    }
    b.uv.hasOff = mPackParams.hasOff;
    const LoadedTexture lt = LoadedTexAt(mRdp.texture_tile[tile].tmem_index);
    b.srcAddr = lt.addr;
    b.srcBytes = lt.size_bytes;
    b.imm = lt.immutable;
    b.cls = static_cast<uint8_t>(lt.addr != nullptr && lt.immutable == 0
                                     ? gdx_gfx_tmem_source_class(reinterpret_cast<uintptr_t>(lt.addr), lt.size_bytes)
                                     : GDX_TMEMSRC_UNKNOWN);
    gdx_gfx_tmem_source_stamp(b.stamp);
    b.checkedSeq = mNrr->seq;
    return true;
}

/* Which colour registers feed the current shader's TexEnv constants (Backend::BatchConstInputs: the
 * inputs bound from vertex 0), as NRR_CONST_* bits. */
GDX_COLD uint8_t NrrConstRegs(Backend* backend, const Combiner* comb, bool* exotic) {
    uint32_t rgbMask = 0, alphaMask = 0;
    *exotic = false;
    if (!backend->BatchConstInputs(&rgbMask, &alphaMask)) {
        *exotic = true;
        return NRR_CONST_PRIM | NRR_CONST_ENV | NRR_CONST_SHADE; /* unknown: any change splits */
    }
    uint8_t bits = 0;
    for (int ch = 0; ch < 2; ch++) {
        const uint32_t mask = ch == 0 ? rgbMask : alphaMask;
        for (int j = 0; j < 7; j++) {
            if (((mask >> j) & 1u) == 0) {
                continue;
            }
            const uint8_t m = comb->shader_input_mapping[ch][j];
            if (m == CCMUX_PRIMITIVE || m == CCMUX_PRIMITIVE_ALPHA || m == CCMUX_PRIM_LOD_FRAC ||
                (ch == 1 && m == ACMUX_PRIM_LOD_FRAC)) {
                bits |= NRR_CONST_PRIM;
            } else if (m == CCMUX_ENVIRONMENT || m == CCMUX_ENV_ALPHA) {
                bits |= NRR_CONST_ENV;
            } else if (m == CCMUX_SHADE || m == CCMUX_SHADE_ALPHA) {
                bits |= NRR_CONST_SHADE;
            } else if (m != CCMUX_TEXEL0 && m != CCMUX_TEXEL1 && m != CCMUX_0 && m != CCMUX_1) {
                bits |= NRR_CONST_PRIM | NRR_CONST_ENV | NRR_CONST_SHADE; /* key / convert / lod: be safe */
                *exotic = true;
            }
        }
    }
    return bits;
}

/* The core's state block for a captured block (cold; again after a state flush). */
/* The texture a block's core state binds: its own id, or -- for an atlas view under
 * [native] nrr_hud_wide bit 2 -- the page every view on it binds (NRR_TEX_PAGE): one core state
 * per page, not per view (a screen of font glyphs is ~40 views of one page; the core's 128-entry
 * table filled and every glyph fell back to the stock rectangle). Exact: the core resolves a
 * view to its page's C3D_Tex (GfxRenderingAPIC3D::NrrTex), the view offset rides the UVs. */
GDX_COLD uint32_t Interp::NrrStateTex(const NrrBlock& b) const {
    if (!b.usesTex) {
        return NRR_TEX_NONE;
    }
    const int page = (mNrrWide & kWidePageStates) != 0 ? mBackend->TexPage(b.texId) : -1;
    return page >= 0 ? (NRR_TEX_PAGE | static_cast<uint32_t>(page)) : b.texId;
}

/* A key's core state block: the core's shared table, or -- once that is full (the other
 * categories' states and earlier screens' fill it: the post-race screens met a full table and
 * drew every new glyph state the stock way) -- one of the HUD's own (NrrStateBuildInto, as nrr_fx
 * does), allocated on first need. A core flush (new generation) empties them too. */
GDX_COLD const NrrState* Interp::NrrHudState(const NrrStateKey& k) {
    const NrrState* st = NrrStateGet(&k);
    if (st == nullptr && NrrCoreTableFlush()) {
        st = NrrStateGet(&k);
    }
    return st != nullptr ? st : NrrOwnState(k);
}

/* The core's state table is append-only: a race's and earlier screens' states stay in it until a
 * mode change clears the texture cache, so the post-race screens met it full. The HUD flushes it
 * (every category re-resolves its blocks on the generation change, as after a texture-cache
 * clear) -- at most once per kCoreFlushEvery tasks, so a working set the table cannot hold falls
 * to the HUD's own states (NrrOwnState) instead of rebuilding every frame. The hand-off closes
 * first: a rebuilt state may reuse the address of the one the open batch applied. */
GDX_COLD bool Interp::NrrCoreTableFlush() {
    NrrHud& N = *mNrr;
    if (N.coreFlushed && N.seq - N.coreFlushSeq < kCoreFlushEvery) {
        return false;
    }
    NrrHudHotClose();
    gdx3ds_nrr_state_flush();
    N.coreFlushed = true;
    N.coreFlushSeq = N.seq;
    N.w.coreFlushes++;
    return true;
}

GDX_COLD const NrrState* Interp::NrrOwnState(const NrrStateKey& k) {
    NrrHud& N = *mNrr;
    const uint32_t gen = NrrStateGeneration();
    if (N.ownGen != gen) {
        N.nOwn = 0;
        N.ownGen = gen;
    }
    for (uint32_t i = 0; i < N.nOwn; i++) {
        if (memcmp(&N.ownKey[i], &k, sizeof(NrrStateKey)) == 0) {
            return N.own[i];
        }
    }
    if (N.nOwn >= kOwnStates) {
        N.w.ownFull++;
        return nullptr;
    }
    if (N.ownMem[N.nOwn] == nullptr) {
        N.ownMem[N.nOwn] = ::operator new(NrrStateBytes(), std::nothrow);
    }
    const NrrState* st = N.ownMem[N.nOwn] != nullptr ? NrrStateBuildInto(&k, N.ownMem[N.nOwn]) : nullptr;
    if (st == nullptr) {
        N.w.ownFull++;
        return nullptr;
    }
    N.ownKey[N.nOwn] = k;
    N.own[N.nOwn] = st;
    N.nOwn++;
    return st;
}

GDX_COLD bool Interp::NrrResolveState(NrrBlock& b) {
    if (b.stock != 0) {
        return false;
    }
    NrrStateKey k;
    memset(&k, 0, sizeof(k));
    k.shaderId0 = b.ds.comb->shader_id0;
    k.shaderId1 = b.ds.comb->shader_id1 | static_cast<uint64_t>(b.ds.tm) * SHOPT_TEXEL0_CLAMP_S;
    k.tex[0] = NrrStateTex(b);
    k.tex[1] = NRR_TEX_NONE;
    k.texLinear[0] = b.smpLinear;
    k.texCms[0] = b.smpCms;
    k.texCmt[0] = b.smpCmt;
    k.program = NRR_PROG_2D;
    k.depthTest = (b.depth & 1u) != 0 ? 1 : 0;
    k.depthWrite = (b.depth & 2u) != 0 ? 1 : 0;
    k.decal = b.decal;
    k.blend = b.alpha != 0 ? NRR_BLEND_ALPHA : NRR_BLEND_OFF;
    k.cull = NRR_CULL_NONE;
    k.alphaTest = NRR_AT_OFF; /* the shader's own edge / threshold options select it (core BuildEffect) */
    /* == ApplyAlphaTest's reference: SetCurrentAlphaCompareThreshold(a / 255.0f), then * 255 */
    k.alphaRef = static_cast<uint8_t>(static_cast<int>((static_cast<float>(b.key.w[7] & 0xFFu) / 255.0f) * 255.0f));
    if (b.scissor.width > 0x7FFFu || b.scissor.height > 0x7FFFu) {
        return false; /* a degenerate N64 scissor (lry < uly wraps the height): the core's key cannot carry it */
    }
    k.scissor[0] = b.scissor.x;
    k.scissor[1] = b.scissor.y;
    k.scissor[2] = static_cast<int16_t>(b.scissor.width);
    k.scissor[3] = static_cast<int16_t>(b.scissor.height);
    b.batchKey = k;
    const int page = b.usesTex ? mBackend->TexPage(b.texId) : -1;
    if (page >= 0) {
        b.batchKey.tex[0] = NRR_TEX_PAGE | static_cast<uint32_t>(page);
    }
    b.st = NrrHudState(k);
    b.stNoSc = nullptr;
    b.stGen = NrrStateGeneration();
    b.batchCls = NrrInternClass(b.batchKey);
    return b.st != nullptr;
}

/* SpTri's gdxInput switch for the block's vertex colour / alpha inputs, resolved: a register or a
 * constant (colSrc / alphaSrc), the corner's vertex colour (nullptr), or the full switch (cold). */
GDX_COLD void Interp::NrrSelectInputs(NrrBlock& b) {
    const Combiner* comb = b.ds.comb;
    const int ci = b.params.vtxColorInput - 1;
    b.colSrc = &kRgbaOne;
    if (ci >= 0) {
        switch (comb->shader_input_mapping[0][ci]) {
            case CCMUX_PRIMITIVE: b.colSrc = &mRdp.prim_color; break;
            case CCMUX_ENVIRONMENT: b.colSrc = &mRdp.env_color; break;
            case CCMUX_SHADE: b.colSrc = nullptr; break;
            case CCMUX_SHADE_ALPHA:
            case CCMUX_PRIMITIVE_ALPHA:
            case CCMUX_ENV_ALPHA:
            case CCMUX_PRIM_LOD_FRAC:
            case CCMUX_LOD_FRACTION:
            case CCMUX_KEY_CENTER:
            case CCMUX_KEY_SCALE:
            case CCMUX_CONVERT_K4:
            case CCMUX_CONVERT_K5: b.coldColour = 1; break;
            default: b.colSrc = &kRgbaZero; break;
        }
    }
    const int ai = b.params.vtxAlphaInput - 1;
    b.alphaSrc = &kRgbaOne;
    if (b.params.fogFactorToAlpha) {
        b.coldColour = 1;
    } else if (b.params.wantVtxAlpha) {
        const uint8_t m = ai >= 0 ? comb->shader_input_mapping[1][ai] : 0xFFu;
        if (m == CCMUX_PRIMITIVE) {
            b.alphaSrc = &mRdp.prim_color;
        } else if (m == CCMUX_ENVIRONMENT) {
            b.alphaSrc = &mRdp.env_color;
        } else if (m == CCMUX_SHADE || m == CCMUX_SHADE_ALPHA) {
            b.alphaSrc = nullptr; /* no fog here (a fog block is a stock key): the vertex alpha */
        } else if (m == CCMUX_PRIMITIVE_ALPHA || m == CCMUX_ENV_ALPHA || m == CCMUX_PRIM_LOD_FRAC ||
                   m == CCMUX_CONVERT_K4 || m == CCMUX_CONVERT_K5 || (m < 3 && m != ACMUX_PRIM_LOD_FRAC)) {
            b.alphaSrc = &kRgbaZero; /* gdxInput leaves alpha 0 for these */
        } else {
            b.coldColour = 1;
        }
    }
}

/* The block must reproduce the stock rectangle's two triangles bit for bit. */
GDX_COLD bool Interp::NrrBlockSelfCheck(const NrrBlock& b, const Cmd& c, const GdxHudWord* t, bool fill) {
    NrrHud& N = *mNrr;
    float mine[kQuadFloats];
    int32_t e[4];
    float st[4];
    bool flip;
    if (!NrrHudHotGeom(b, c, fill ? nullptr : t, e, st, &flip)) {
        return false;
    }
    NrrHudHotCorners(b, e, st, flip, mine);
    const float* got = mPackDst + (mBufNumTris - 2) * (3 * kStride);
    if (memcmp(got, mine, sizeof(mine)) == 0) {
        return true;
    }
    N.w.selfBad++;
    N.sesSelfBad++;
    if (N.selfBadLogs < 8) {
        N.selfBadLogs++;
        for (uint32_t v = 0; v < 6; v++) {
            const float* g = got + v * kStride;
            const float* p = mine + v * kStride;
            if (memcmp(g, p, kStride * sizeof(float)) != 0) {
                GDXN_LOG("[nrrhud] selfcheck MISMATCH fill=%d v=%u got=%.6f,%.6f,%.6f,%.6f uv=%.6f,%.6f "
                         "c=%.4f,%.4f,%.4f,%.4f pred=%.6f,%.6f,%.6f,%.6f uv=%.6f,%.6f c=%.4f,%.4f,%.4f,%.4f",
                         fill ? 1 : 0, static_cast<unsigned>(v), g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8],
                         g[9], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
                break;
            }
        }
    }
    return false;
}

/* The pending list is full (a screen of font glyphs loads one per rectangle into the same TMEM
 * words): the loads a later one covers whole are dead, so they go; if every one is live, all of
 * them settle into the interpreter's TMEM (what a stock reader would do). Room for one after. */
GDX_COLD void Interp::NrrPendMakeRoom() {
    NrrHud& N = *mNrr;
    uint32_t keep = 0;
    for (uint32_t k = 0; k < N.nPend; k++) {
        if (N.pend[k].lo != N.pend[k].hi) {
            N.pend[keep++] = N.pend[k];
        }
    }
    N.nPend = keep;
    if (N.nPend >= kPend) {
        NrrMaterializeAll();
    }
}

/* One pending load, for real: DpLoadBlock under the load's own SETTIMG and TMEM address. */
GDX_COLD void Interp::NrrMaterializeAt(uint32_t i) {
    NrrHud& N = *mNrr;
    const NrrHud::Pend p = N.pend[i];
    if (p.lo == p.hi) {
        return; /* dead: a later pending load writes all of it */
    }
    const uint8_t tileIdx = static_cast<uint8_t>(C1(p.lw1, 24, 3));
    const bool wide = (p.timgW0 & 0xFFFu) != 0; /* a SETTIMG width > 1: its record carries the line pitch */
    const Cmd timg = { wide ? NrrWideTimgWord(p.timgW0) : p.timgW0, p.timgW1, kHostTag };
    const auto savedTl = mRdp.texture_to_load;
    const uint16_t savedIndex = mRdp.texture_tile[tileIdx].tmem_index;
    const bool savedVirtual = mHudVirtualLoad;
    mHudVirtualLoad = false; /* a hud_direct load in flight must not turn this copy into its debt */
    N.materializing = true;
    DpSetTextureImage(C0(timg.w0, 21, 3), C0(timg.w0, 19, 2), C0(timg.w0, 0, 12) + 1, timg);
    mRdp.texture_tile[tileIdx].tmem_index = p.lo;
    mLoadLineForce = wide ? NrrWideLine(p.timgW0) : 0u;
    const uint32_t seg = p.lw0 >> 24; /* a resolved segmented operand: its flags (NrrSegResolve) */
    if (seg != OP_LOADBLOCK) {
        mLoadSrcForce = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(p.timgW1));
        mLoadSrcFlags = static_cast<uint8_t>(seg);
    }
    DpLoadBlock(tileIdx, C0(p.lw0, 12, 12), C0(p.lw0, 0, 12), C1(p.lw1, 12, 12), C1(p.lw1, 0, 12));
    mLoadLineForce = 0;
    mLoadSrcForce = nullptr;
    mRdp.texture_tile[tileIdx].tmem_index = savedIndex;
    mRdp.texture_to_load = savedTl;
    N.materializing = false;
    mHudVirtualLoad = savedVirtual;
    N.w.matLoads++;
}

/* Every pending load into the interpreter's TMEM, oldest first. */
GDX_COLD void Interp::NrrMaterializeAll() {
    if (mNrr == nullptr || mNrr->nPend == 0) {
        return;
    }
    NrrHud& N = *mNrr;
    N.w.mats++;
    for (uint32_t i = 0; i < N.nPend; i++) {
        NrrMaterializeAt(i);
    }
    N.nPend = 0;
    NrrUpdateLive();
}

/* An interpreter write to TMEM [off, off + n) (TmemLoadIsRedundant, before its same-content
 * decision): the pending loads it covers whole are dead, one it only partly covers is
 * materialised first (with everything pending, in order), so the bytes land in the stock order;
 * the NRR's identities over the range are gone. */
GDX_COLD void Interp::NrrTmemWrite(uint32_t off, uint32_t n) {
    NrrHud& N = *mNrr;
    if (N.materializing) {
        return;
    }
    const uint32_t lo = off >> 3;
    const uint32_t hi = (off + n + 7u) >> 3 < 512u ? (off + n + 7u) >> 3 : 512u;
    bool partial = false;
    for (uint32_t k = 0; k < N.nPend;) {
        const NrrHud::Pend& p = N.pend[k];
        if (p.hi <= lo || p.lo >= hi) {
            k++;
        } else if (p.lo >= lo && p.hi <= hi) {
            NrrDropPendingAt(k);
        } else {
            partial = true;
            k++;
        }
    }
    if (partial) {
        N.w.partial++;
        NrrMaterializeAll();
    }
    for (uint32_t k = 0; k < kSlots; k++) {
        if (N.slots[k].lo < hi && N.slots[k].hi > lo) {
            N.slots[k].hi = N.slots[k].lo; /* gone */
        }
    }
    NrrUpdateLive();
}

GDX_COLD void Interp::NrrDropPendingAt(uint32_t i) {
    NrrHud& N = *mNrr;
    for (uint32_t k = i + 1u; k < N.nPend; k++) {
        N.pend[k - 1u] = N.pend[k];
    }
    N.nPend--;
}

/* An interpreter reader of `tile`'s TMEM: settle what is pending if a pending load wrote it. */
GDX_COLD void Interp::NrrTmemRead(int tile) {
    if (mTexOv != nullptr) {
        return; /* a machines_texskip window serves its own recorded view */
    }
    const uint16_t wd = mRdp.texture_tile[tile & 7].tmem_index;
    NrrHud& N = *mNrr;
    for (uint32_t k = 0; k < N.nPend; k++) {
        if (wd >= N.pend[k].lo && wd < N.pend[k].hi) {
            N.w.staleRead++;
            NrrMaterializeAll();
            return;
        }
    }
}

GDX_COLD void Interp::NrrUpdateLive() {
    const NrrHud& N = *mNrr;
    bool live = N.nPend != 0;
    for (uint32_t k = 0; k < kSlots && !live; k++) {
        live = N.slots[k].hi != N.slots[k].lo;
    }
    mNrrTmemLive = live;
}

/* [native] nrr_hud=2 (verify): the run executes the stock way, word by word (Exec: the
 * interpreter's own handlers draw every rectangle and load every texture), and what the NRR
 * would have done is checked against it:
 *   - with a program for the run (its skeleton and entry snapshot match), op by op: an ST op's
 *     stores (and the texture arm / flush counter they trigger) are applied, the registers saved,
 *     rolled back, the handler run: the two register images (NrrRegRegions, byte for byte) and
 *     the counter must match (reg); an LD op's record must be the one the load computes now (ld);
 *     an RT / FL op's block must be the block the rectangle's key names now, and its six packed
 *     vertices the stock rectangle's two triangles bit for bit, the state it names (texture id,
 *     sampler, shader, depth / decal / blend, scissor) what the stock path bound, its cached TexEnv
 *     constants HudPackAux's answer (rect, aux); an EX op is checked per word;
 *   - without one, per word (compiled stores, key -> block) while the run's program is recorded
 *     as the per-word decoder would record it.
 * A word or key seen for the first time is compiled / captured instead (new); a rectangle with no
 * identity or a stock key is a skip. Receipt: [nrrhud] ... vfy reg= ld= rect= aux= prog=. */
GDX_COLD void Interp::NrrVerifyRun(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    N.w.runs++;
    N.w.words += n;
    N.runWords = w;
    N.runN = n;
    N.last = nullptr;
    N.dirty = true;
    NrrRunEpochs();
    const NrrProg* p = mTexOv == nullptr ? NrrHudHotProg(w, n) : nullptr;
    if (p != nullptr) {
        N.vfy.progRuns++;
        NrrVerifyProg(*p, w, n);
        return;
    }
    NrrRecBegin(n <= kProgMaxWords);
    for (uint32_t i = 0; i < n;) {
        i = NrrVerifyWord(w, n, i, N.recOk);
    }
    if (N.recOk && N.progEpoch == N.recEpoch) {
        NrrProgStore(w, n);
    }
}

/* One word, per word (see NrrVerifyRun); `rec`: record its op. Returns the next word's index. */
GDX_COLD uint32_t Interp::NrrVerifyWord(const GdxHudWord* w, uint32_t n, uint32_t i, bool rec) {
    NrrHud& N = *mNrr;
    const Cmd c = { w[i].w0, w[i].w1, w[i].w1hi };
    const uint8_t op = Opcode(c.w0);
    if ((op == OP_TEXRECT || op == OP_TEXRECTFLIP) && i + 2 < n) {
        return NrrVerifyRect(c, w + i, i, nullptr, rec);
    }
    if (op == OP_FILLRECT) {
        return NrrVerifyRect(c, nullptr, i, nullptr, rec);
    }
    if (op == OP_LOADBLOCK) {
        return NrrVerifyLoad(c, i, nullptr, rec);
    }
    if (op == OP_LOADTLUT || op == OP_EXTRAGEOMETRYMODE || op == OP_TRI1 || op == OP_TRI2 || op == OP_QUAD ||
        op == OP_TEXRECT || op == OP_TEXRECTFLIP) {
        const uint32_t next = NrrExecWord(c, i);
        if (op == OP_LOADTLUT) {
            NrrNoteTlut(c);
        } else if (op == OP_EXTRAGEOMETRYMODE) {
            N.envEgm = mRsp.extra_geometry_mode;
            NrrXMode();
        }
        if (rec) {
            if (op == OP_LOADTLUT) {
                NrrRecTl(i, c);
            } else {
                NrrRecEx(i);
            }
        }
        return next;
    }
    if (op == OP_DL) {
        return NrrVerifyDl(w, n, i, nullptr, rec); /* n60hudx nrr_hud_kept */
    }
    if (GDX_UNLIKELY(mNiMenuRun) && op == OP_LOADTILE) {
        const uint32_t next = NrrVerifyReg(c, i, nullptr, 0, rec);
        NrrNoteLoadTile(c); /* nimenu: as the decoder does */
        return next;
    }
    return NrrVerifyReg(c, i, nullptr, 0, rec);
}

/* n60hudx nrr_hud_kept, verify: a G_DL per word compiles as the decoder does (new); an SL op's stores are
 * checked against the stock walk of its list: the register regions after the walk (from the entry S)
 * vs after the op's stores (from S), byte for byte, the texture flags included. The stock result stays. */
GDX_COLD uint32_t Interp::NrrVerifyDl(const GdxHudWord* w, uint32_t n, uint32_t i, const uint32_t* opPay, bool rec) {
    NrrHud& N = *mNrr;
    const Cmd c = { w[i].w0, w[i].w1, w[i].w1hi };
    if (opPay == nullptr) {
        N.vfy.regNew++;
        return NrrDecDl(w, n, i, rec, nullptr);
    }
    N.last = nullptr;
    N.dirty = true;
    if (N.drew) {
        mRdp.viewport_or_scissor_changed = true;
        N.drew = false;
    }
    ListRef sub;
    bool taken = false;
    if (!ResolveDlAsExec(c, sub, &taken) || taken) {
        N.vfy.rectSkip++;
        return i + 1;
    }
    const uint32_t ref = static_cast<uint32_t>(sub.stride) | (sub.isBig ? 0x100u : 0u) | (sub.isF3d ? 0x200u : 0u);
    const bool same = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sub.base)) == opPay[0] &&
                      static_cast<uint32_t>(sub.limit) == opPay[1] && ref == opPay[2] &&
                      memcmp(sub.base, opPay + 4, (opPay[3] & 0xFFFFu) * sub.stride) == 0;
    uint8_t* base[3];
    uint32_t len[3];
    if (!same || mBufNumTris != 0 || mTexOv != nullptr || NrrRegRegions(base, len) == 0) {
        N.vfy.rectSkip++; /* the hot path would miss here (another list): nothing to compare */
        WalkNestedList(sub);
        return i + 1;
    }
    uint32_t flushSaved[FLUSH_COUNT];
    memcpy(flushSaved, mStats.flush, sizeof(flushSaved));
    NrrRegCopy(base, len, N.fxS, false, false);
    WalkNestedList(sub); /* stock */
    NrrRegCopy(base, len, N.fxR, false, false);
    NrrRegCopy(base, len, N.fxS, true, false);
    NrrHudHotGroups(opPay + 4 + NrrSlRawWords(opPay), opPay + (opPay[3] >> 16));
    NrrRegCopy(base, len, N.fxP, false, false);
    NrrRegCopy(base, len, N.fxR, true, false); /* the stock result stays */
    memcpy(mStats.flush, flushSaved, sizeof(flushSaved));
    const uint32_t total = len[0] + len[1] + len[2];
    if (memcmp(N.fxR, N.fxP, total) == 0) {
        N.vfy.regOk++;
    } else {
        uint32_t at = 0;
        while (at < total && N.fxR[at] == N.fxP[at]) {
            at++;
        }
        NrrVerifyBad("sl stores", c, at, (static_cast<uint32_t>(N.fxR[at]) << 8) | N.fxP[at]);
        N.vfy.regBad++;
        N.vfySes.regBad++;
    }
    return i + 1;
}

/* A program's ops against the stock execution (see NrrVerifyRun). */
GDX_COLD void Interp::NrrVerifyProg(const NrrProg& p, const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    const uint32_t* op = N.pool + p.ops;
    const uint32_t* const end = op + p.opWords;
    uint32_t i = 0;
    while (op < end && i < n) {
        const uint32_t hdr = *op++;
        const uint32_t kind = hdr & 7u;
        const uint32_t at = hdr >> 16;
        const Cmd c = { w[at].w0, w[at].w1, w[at].w1hi };
        if (at != i) {
            NrrVerifyBad("prog order", c, at, i);
            N.vfy.regBad++;
            N.vfySes.regBad++;
            i = at;
        }
        const uint32_t* m = kOpCheck[kind];
        if ((((c.w0 ^ op[0]) & m[0]) | ((c.w1 ^ op[1]) & m[1]) | ((c.w1hi ^ op[2]) & m[2])) != 0) {
            NrrVerifyBad("prog word", c, i, kind);
            N.vfy.regBad++;
            N.vfySes.regBad++;
        }
        op += 3;
        if (kind == kOpSt) {
            i = NrrVerifyReg(c, i, op, hdr, false);
            op += 3u * ((hdr >> 4) & 15u);
        } else if (kind == kOpCol) {
            /* the COL op as the stores it makes for this word, through the ST check */
            uint32_t st[6] = { op[0], 0xFFFFFFFFu, __builtin_bswap32(c.w1), op[1], 0xFFu << op[2],
                               (c.w0 & 0xFFu) << op[2] };
            if ((hdr & kOpColPd) != 0) {
                st[1] = 0xFFFFu << op[2];
                st[2] = ((c.w1 >> 16) & 0x7FFFu) << op[2];
            }
            const uint32_t nst = op[1] != 0 ? 2u : 1u;
            i = NrrVerifyReg(c, i, st, kOpSt | (nst << 4) | (hdr & (31u << 9)), false);
            op += 3;
        } else if (kind == kOpLd) {
            i = NrrVerifyLoad(c, i, op, false);
            op += kLdWords;
        } else if (kind == kOpSl) {
            i = NrrVerifyDl(w, n, i, op, false);
            op += op[3] >> 16;
        } else if (kind == kOpRt || kind == kOpFl) {
            NrrBlock* b = reinterpret_cast<NrrBlock*>(op[0]);
            if (kind == kOpRt && !NrrHudHotIdent(op + 1, mRdp.texture_tile[C1(c.w1, 24, 3)].tmem_index)) {
                NrrVerifyBad("prog ident", c, i, 0);
                N.vfy.rectBad++;
                N.vfySes.rectBad++;
            }
            op += kind == kOpRt ? kRtWords : 1u;
            i = NrrVerifyRect(c, kind == kOpRt ? w + i : nullptr, i, b, false);
        } else {
            i = NrrVerifyWord(w, n, i, false);
        }
    }
    if (op != end || i != n) {
        NrrVerifyBad("prog length", Cmd{ 0, 0, 0 }, i, n);
        N.vfy.regBad++;
        N.vfySes.regBad++;
        while (i < n) {
            i = NrrVerifyWord(w, n, i, false);
        }
    }
}

GDX_COLD void Interp::NrrVerifyBad(const char* what, const Cmd& c, uint32_t a, uint32_t b) {
    NrrHud& N = *mNrr;
    if (N.vfyLogs < 16) {
        N.vfyLogs++;
        GDXN_LOG("[nrrhud] vfy MISMATCH %s w0=%08lx w1=%08lx a=%lu b=%lu", what, static_cast<unsigned long>(c.w0),
                 static_cast<unsigned long>(c.w1), static_cast<unsigned long>(a), static_cast<unsigned long>(b));
    }
}

/* A LOADBLOCK, for real; the identity the hot path would record (before the load: its operand).
 * `opRec`: an LD op's record, which must be the one computed now. */
GDX_COLD uint32_t Interp::NrrVerifyLoad(const Cmd& c, uint32_t i, const uint32_t* opRec, bool rec) {
    NrrHud& N = *mNrr;
    uint32_t r[kLdWords];
    const bool virt = NrrLoadRecord(c, r);
    if (opRec != nullptr) {
        if (virt && memcmp(r, opRec, sizeof(r)) == 0) {
            N.vfy.ldOk++;
        } else {
            N.vfy.ldBad++;
            N.vfySes.ldBad++;
            NrrVerifyBad("ld", c, virt ? 1u : 0u, opRec[4]);
        }
    }
    const uint32_t skips0 = mStats.tmemSkips;
    const uint32_t next = NrrExecWord(c, i);
    if (virt && ((r[0] & 0xFFFu) != 0 || NrrSegResolved(r)) && mStats.tmemSkips == skips0) {
        NrrVerifyLoadedAs(c, r); /* the stock load's pitch / source are the ones the record carries */
    }
    if (virt) {
        NrrHudHotSlot(r[4] & 0xFFFFu, r[4] >> 16, r);
        mNrrTmemLive = true;
    }
    N.vfy.loads++;
    if (rec) {
        if (virt) {
            NrrRecLoad(i, r, c);
        } else {
            NrrRecEx(i);
        }
    }
    return next;
}

/* A wide or segmented load the stock way just stored its record at the load's TMEM address: its
 * pitch must be the one the virtual record carries (what NrrMaterializeAt would force), and a
 * segmented one's source and classification the ones it resolved to. */
GDX_COLD void Interp::NrrVerifyLoadedAs(const Cmd& c, const uint32_t* r) {
    NrrHud& N = *mNrr;
    const LoadedTexture lt = LoadedTexAt(static_cast<uint16_t>(r[4] & 0xFFFFu));
    const uint32_t line = (r[0] & 0xFFFu) != 0 ? NrrWideLine(r[0]) : lt.line_size_bytes;
    bool same = lt.line_size_bytes == line;
    if (NrrSegResolved(r)) {
        const uint32_t flags = r[2] >> 24;
        const uintptr_t base = static_cast<uintptr_t>(r[1]);
        const uintptr_t at = base + static_cast<uintptr_t>(C0(c.w0, 0, 12)) * line +
                             NrrTexelBytes(C0(c.w0, 12, 12), mRdp.texture_to_load.siz);
        same = same && reinterpret_cast<uintptr_t>(lt.addr) == at &&
               (lt.immutable != 0) == ((flags & kSegImmutable) != 0) &&
               (lt.native_rgba16 != 0) == ((flags & kSegNative) != 0);
    }
    if (same) {
        N.vfy.ldOk++;
        return;
    }
    N.vfy.ldBad++;
    N.vfySes.ldBad++;
    NrrVerifyBad("ldsrc", c, lt.line_size_bytes, line);
}

/* A register word: compiled stores (the memo's, or an ST op's `opSt` under `opHdr`) vs
 * the handler. The NRR runs a register word with no interpreter batch open; the stock rectangles
 * of a verify run leave one, so it is drained first (a split batch: the same pixels). */
GDX_COLD uint32_t Interp::NrrVerifyReg(const Cmd& c, uint32_t i, const uint32_t* opSt, uint32_t opHdr, bool rec) {
    NrrHud& N = *mNrr;
    const uint8_t op = Opcode(c.w0);
    if (mBufNumTris != 0 && NrrRegCompilable(op)) {
        Flush(FLUSH_HOOK);
    }
    uint32_t h = (c.w0 * 0x9E3779B1u) ^ (c.w1 * 0x85EBCA77u);
    h ^= h >> 15;
    const NrrRegFx* fx = nullptr;
    for (uint32_t p = 0; p < kRegProbe && fx == nullptr && opSt == nullptr; p++) {
        const NrrRegFx& e = N.regs[(h + p) & (kRegFx - 1u)];
        if (e.used == 0) {
            break;
        }
        if (e.w0 == c.w0 && e.w1 == c.w1 && e.w1hi == c.w1hi) {
            fx = &e;
        }
    }
    const bool plain = mBufNumTris == 0 && mTexOv == nullptr;
    if (opSt == nullptr && (fx == nullptr || fx->n > kFxStores || !plain)) {
        if (fx == nullptr && NrrRegCompilable(op)) {
            N.vfy.regNew++;
        }
        const NrrRegFx* made = nullptr;
        const uint32_t next = NrrRegCold(c, i, h, &made); /* compiles it (its handler runs), or runs it */
        if (rec) {
            NrrVerifyRecReg(c, i, h);
        }
        return next;
    }
    uint32_t st[3 * kFxStores];
    const uint32_t hdr = opSt != nullptr ? opHdr : NrrFxHeader(*fx, st);
    if (opSt != nullptr) {
        for (uint32_t k = 0; k < 3u * ((hdr >> 4) & 15u); k++) {
            st[k] = opSt[k];
        }
    }
    uint8_t* base[3];
    uint32_t len[3];
    if (NrrRegRegions(base, len) == 0) {
        return NrrExecWord(c, i);
    }
    const uint32_t total = len[0] + len[1] + len[2];
    const uint32_t why = (hdr >> 9) & 31u;
    const uint32_t flush0 = mStats.flush[why];
    NrrRegCopy(base, len, N.fxS, false, false);
    (void)NrrHudHotStores(hdr, st);
    const uint32_t predicted = mStats.flush[why] - flush0;
    NrrRegCopy(base, len, N.fxP, false, false);
    NrrRegCopy(base, len, N.fxS, true, false); /* rolled back */
    mStats.flush[why] = flush0;
    const uint32_t next = NrrExecWord(c, i);
    NrrRegCopy(base, len, N.fxR, false, false);
    const bool flushSame = !plain || (mStats.flush[why] - flush0) == predicted;
    if (memcmp(N.fxR, N.fxP, total) == 0 && flushSame) {
        N.vfy.regOk++;
    } else {
        N.vfy.regBad++;
        N.vfySes.regBad++;
        uint32_t at = 0;
        while (at < total && N.fxR[at] == N.fxP[at]) {
            at++;
        }
        NrrVerifyBad(flushSame ? "reg" : "reg flush", c, at, at < total ? (N.fxR[at] << 8 | N.fxP[at]) : 0u);
    }
    if (rec) {
        NrrRecRegWord(c, i, fx, true); /* as the per-word decoder records it (a colour word: COL) */
    }
    return next;
}

/* The ST op of a compiled word: its header and stores. */
GDX_COLD uint32_t Interp::NrrFxHeader(const NrrRegFx& fx, uint32_t st[3 * kFxStores]) const {
    for (uint32_t k = 0; k < fx.n; k++) {
        st[k * 3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fx.dst[k]));
        st[k * 3 + 1] = fx.mask[k];
        st[k * 3 + 2] = fx.val[k];
    }
    return kOpSt | (static_cast<uint32_t>(fx.n) << 4) | (fx.arm != 0 ? kOpArm : 0u) |
           (static_cast<uint32_t>(fx.why) << 9);
}

/* After a verify word compiled (or not): its op, as the per-word decoder would record it. */
GDX_COLD void Interp::NrrVerifyRecReg(const Cmd& c, uint32_t i, uint32_t h) {
    NrrHud& N = *mNrr;
    for (uint32_t p = 0; p < kRegProbe; p++) {
        const NrrRegFx& e = N.regs[(h + p) & (kRegFx - 1u)];
        if (e.used == 0) {
            break;
        }
        if (e.w0 == c.w0 && e.w1 == c.w1 && e.w1hi == c.w1hi) {
            NrrRecRegWord(c, i, e.n <= kFxStores ? &e : nullptr, mTexOv == nullptr);
            return;
        }
    }
    NrrRecRegWord(c, i, nullptr, mTexOv == nullptr);
}

/* A rectangle: the block's prediction vs the stock rectangle (see NrrVerifyRun). `opBlock`: an RT /
 * FL op's block, which must be the one the key names now. */
GDX_COLD uint32_t Interp::NrrVerifyRect(const Cmd& c, const GdxHudWord* t, uint32_t i, NrrBlock* opBlock, bool rec) {
    NrrHud& N = *mNrr;
    const bool fill = t == nullptr;
    if (fill && mRdp.color_image_address == mRdp.z_buf_address) {
        if (rec) {
            NrrRecEx(i);
        }
        return NrrExecWord(c, i);
    }
    const uint8_t tile = fill ? mRdp.first_tile_index : static_cast<uint8_t>(C1(c.w1, 24, 3));
    NrrKey k;
    uint32_t h = 0;
    const bool known = NrrDecKey(k, tile, fill);
    NrrBlock* b = known ? NrrDecFind(k, &h) : nullptr;
    if (opBlock != nullptr && opBlock != b) {
        N.vfy.rectBad++;
        N.vfySes.rectBad++;
        NrrVerifyBad("rect block", c, known ? 1u : 0u, b != nullptr ? 1u : 0u);
    }
    const bool fresh = b != nullptr && b->stock == 0 && NrrBlockFresh(*b);
    const bool usable = fresh && (b->stGen == NrrStateGeneration() || NrrResolveState(*b));
    float mine[kQuadFloats];
    bool drawn = false;
    if (usable) {
        int32_t e[4];
        float st[4];
        bool flip;
        drawn = NrrHudHotGeom(*b, c, t, e, st, &flip);
        if (drawn) {
            if (fill) {
                for (int v = kMaxVertices; v < kMaxVertices + 4; v++) {
                    mRsp.loaded_vertices[v].color = mRdp.fill_color; /* == DpFillRectangle, before its draw */
                }
            }
            NrrHudHotCorners(*b, e, st, flip, mine);
            NrrVerifyAux(*b);
        }
    }
    const uint32_t emit0 = mStats.triEmitAll;
    const uint32_t next = NrrExecWord(c, i);
    if (!known || (b != nullptr && b->stock != 0)) {
        N.vfy.rectSkip++;
    } else if (!fresh) {
        N.vfy.rectNew++;
        NrrCaptureAfter(k, h, tile, fill, emit0, c, t);
        b = NrrDecFind(k, &h);
    } else if (!usable || !drawn) {
        N.vfy.rectSkip++;
    } else {
        NrrVerifyRectOut(*b, c, mine, emit0);
    }
    if (rec) {
        if (b != nullptr && known && b->stock == 0 && b->stGen == NrrStateGeneration()) {
            NrrRecRect(i, c, b, tile, fill);
        } else {
            NrrRecEx(i);
        }
    }
    return next;
}

/* The texture the block's core state binds (its id, or its atlas page: NrrStateTex) is the C3D_Tex
 * the stock rectangle's texture id binds. */
GDX_COLD bool Interp::NrrVerifyBindsSame(const NrrBlock& b) const {
    NrrBkTex mine, stock;
    if (!gdx3ds_nrr_bk_tex(NrrStateTex(b), &mine) || !gdx3ds_nrr_bk_tex(b.texId, &stock)) {
        return false;
    }
    return mine.tex == stock.tex;
}

/* The stock rectangle that just drew vs the block's prediction `mine`. */
GDX_COLD void Interp::NrrVerifyRectOut(const NrrBlock& b, const Cmd& c, const float* mine, uint32_t emit0) {
    NrrHud& N = *mNrr;
    const bool emitted = mStats.triEmitAll - emit0 == 2u && mPackBatch && mPackDst != nullptr && mBufNumTris >= 2;
    const bool floatsSame =
        emitted && memcmp(mPackDst + (mBufNumTris - 2) * (3 * kStride), mine, kQuadFloats * sizeof(float)) == 0;
    const bool texSame = !b.usesTex || (mRs.textures[0] >= 0 && mTex[mRs.textures[0]].texture_id == b.texId &&
                                        (mRs.sampler_linear[0] ? 1 : 0) == b.smpLinear &&
                                        mRs.sampler_cms[0] == b.smpCms && mRs.sampler_cmt[0] == b.smpCmt &&
                                        NrrVerifyBindsSame(b));
    const bool stateSame = mRs.shader_program == b.ds.prg && mRs.depth_test_and_mask == b.depth &&
                           mRs.decal_mode == b.decal && mRs.alpha_blend == b.alpha && NrrRectEq(mRs.scissor, b.scissor);
    if (floatsSame && texSame && stateSame) {
        N.vfy.rectOk++;
        return;
    }
    N.vfy.rectBad++;
    N.vfySes.rectBad++;
    if (N.vfyLogs < 16) {
        N.vfyLogs++;
        GDXN_LOG("[nrrhud] vfy MISMATCH rect op=%02X emitted=%d floats=%d tex=%d state=%d texId=%u/%u prg=%p/%p",
                 Opcode(c.w0), emitted ? 1 : 0, floatsSame ? 1 : 0, texSame ? 1 : 0, stateSame ? 1 : 0,
                 static_cast<unsigned>(mRs.textures[0] >= 0 ? mTex[mRs.textures[0]].texture_id : 0xFFFFu),
                 static_cast<unsigned>(b.texId), mRs.shader_program, b.ds.prg);
    }
}

/* A block's TexEnv constants as the hot path computes them (NrrHudHotAux) vs HudPackAux now. */
GDX_COLD void Interp::NrrVerifyAux(const NrrBlock& b) {
    NrrHud& N = *mNrr;
    if (b.auxExotic != 0) {
        return; /* the hot path takes HudPackAux itself */
    }
    const LoadedVertex* lv = &mRsp.loaded_vertices[kMaxVertices];
    float mine[7][4];
    NrrHudHotAux(b, NrrRgbaWord(mRdp.prim_color), NrrRgbaWord(mRdp.env_color), NrrRgbaWord(lv[0].color), mine);
    Gdx3dsVboPackAux aux;
    HudPackAux(b.ds, &lv[0], &aux);
    if (aux.numInputs == b.auxN && memcmp(aux.inputRgba, mine, sizeof(float) * 4u * aux.numInputs) == 0) {
        N.vfy.auxOk++;
        return;
    }
    N.vfy.auxBad++;
    N.vfySes.auxBad++;
    if (N.vfyLogs < 16) {
        N.vfyLogs++;
        GDXN_LOG("[nrrhud] vfy MISMATCH aux n=%u/%u in0=%.4f/%.4f", static_cast<unsigned>(aux.numInputs),
                 static_cast<unsigned>(b.auxN), aux.inputRgba[0][0], mine[0][0]);
    }
}

/* [native] nrr_hud_census=2: a batch's state (bisect diagnostic, unused in the hot path). */
GDX_COLD void Interp::NrrDebugBatch(const NrrBlock& b, const Gdx3dsVboPackAux& aux) {
    const uint32_t fmt = b.key.w[8] & 0xFFu; /* Tile::fmt */
    if (fmt != IM_FMT_IA && fmt != IM_FMT_I) {
        return;
    }
    GDXN_LOG("[nrrhud] dbg nrr batch tex=%u slot=%d st=%p const=%u col=%s alpha=%s depth=%u decal=%u alpha=%u "
             "sci=%d,%d,%u,%u n=%u in0=%.3f,%.3f,%.3f,%.3f in1=%.3f,%.3f,%.3f,%.3f prim=%02x%02x%02x%02x",
             static_cast<unsigned>(b.texId), b.slot, static_cast<const void*>(b.st), b.constRegs,
             b.colSrc == nullptr ? "shade" : (b.colSrc == &mRdp.prim_color ? "prim" : "const"),
             b.alphaSrc == nullptr ? "shade" : (b.alphaSrc == &mRdp.prim_color ? "prim" : "const"), b.depth, b.decal,
             b.alpha, b.scissor.x, b.scissor.y, static_cast<unsigned>(b.scissor.width),
             static_cast<unsigned>(b.scissor.height), aux.numInputs, aux.inputRgba[0][0], aux.inputRgba[0][1],
             aux.inputRgba[0][2], aux.inputRgba[0][3], aux.inputRgba[1][0], aux.inputRgba[1][1], aux.inputRgba[1][2],
             aux.inputRgba[1][3], mRdp.prim_color.r, mRdp.prim_color.g, mRdp.prim_color.b, mRdp.prim_color.a);
}

GDX_COLD void Interp::NrrDebugRect(const NrrBlock& b, const float* v) {
    GDXN_LOG("[nrrhud] dbg nrr rect seq=%u depth=%u decal=%u st=%p tex=%u sci=%d,%d v0=%.4f,%.4f uv=%.4f,%.4f c=%.3f,%.3f,%.3f,%.3f "
             "v2=%.4f,%.4f uv2=%.4f,%.4f verts=%u",
             static_cast<unsigned>(mNrr->seq), b.depth, b.decal, static_cast<const void*>(b.st),
             static_cast<unsigned>(b.texId), b.scissor.x, b.scissor.y, v[0], v[1],
             v[4], v[5], v[6], v[7], v[8], v[9], v[24], v[25], v[28], v[29], static_cast<unsigned>(mNrr->batchVerts));
}

/* The stock side of the same diagnostic: a replayed rectangle from an IA / I tile, as it was drawn. */
GDX_COLD void Interp::NrrDebugStock(const void* runWords, uint32_t at) {
    const GdxHudWord* w = static_cast<const GdxHudWord*>(runWords);
    const uint8_t op = Opcode(w[at].w0);
    if (op != OP_TEXRECT && op != OP_TEXRECTFLIP && op != OP_FILLRECT) {
        return;
    }
    if (!mPackBatch || mBufNumTris < 2) {
        return;
    }
    const float* v = mPackDst + (mBufNumTris - 2) * (3 * kStride);
    GDXN_LOG("[nrrhud] dbg stock rect seq=%u tex=%u slot=%d prg=%p depth=%u decal=%u alpha=%u sci=%d,%d,%u,%u n=%u "
             "in0=%.3f,%.3f,%.3f,%.3f in1=%.3f,%.3f,%.3f,%.3f v0=%.4f,%.4f uv=%.4f,%.4f c=%.3f,%.3f,%.3f,%.3f tris=%u",
             static_cast<unsigned>(mNrr != nullptr ? mNrr->seq : 0u),
             static_cast<unsigned>(mRs.textures[0] >= 0 ? mTex[mRs.textures[0]].texture_id : 0xFFFFu),
             mRs.textures[0], mRs.shader_program, mRs.depth_test_and_mask, mRs.decal_mode, mRs.alpha_blend,
             mRs.scissor.x, mRs.scissor.y, static_cast<unsigned>(mRs.scissor.width),
             static_cast<unsigned>(mRs.scissor.height), mPackAux.numInputs, mPackAux.inputRgba[0][0],
             mPackAux.inputRgba[0][1], mPackAux.inputRgba[0][2], mPackAux.inputRgba[0][3], mPackAux.inputRgba[1][0],
             mPackAux.inputRgba[1][1], mPackAux.inputRgba[1][2], mPackAux.inputRgba[1][3], v[0], v[1], v[4], v[5],
             v[6], v[7], v[8], v[9], static_cast<unsigned>(mBufNumTris));
}

GDX_COLD void Interp::NrrMemoClear() {
    NrrHud& N = *mNrr;
    for (uint32_t i = 0; i < kBlocks; i++) {
        N.blocks[i].used = 0;
    }
    N.nBlocks = 0;
    N.last = nullptr;
    N.nClasses = 0;
    N.batchCls = kNoClass; /* an open batch's class id may be reused: the next rectangle starts a new one */
    N.w.clears++;
    NrrProgClear(); /* their RT / FL ops name blocks */
}

/* Every program goes (their blocks or their stores are stale); a program running sees the epoch move. */
GDX_COLD void Interp::NrrProgClear() {
    NrrHud& N = *mNrr;
    for (uint32_t k = 0; k < kProgs; k++) {
        N.progs[k].n = 0;
    }
    N.nProgs = 0;
    N.poolUsed = 0;
    N.progEpoch++;
    N.memoEpoch++; /* [native] nrr_hud_memo: an op address may name another rectangle now */
    N.rrUsed = 0; /* n60hudx nrr_hud_rr: no program names a record now */
    for (uint32_t k = 0; k < kSlCache; k++) {
        N.slc[k].words = 0; /* n60hudx: their stores were compiled under the old config */
    }
    N.w.progClears++;
}

/* The entry snapshot's words (a program's dependencies are drawn from these): every register a
 * rectangle key or a virtual load reads -- SETTIMG's operand, the tiles, the first tile, the other
 * modes, the combine, grayscale, the blend alpha, the scissor -- and the TLUT identities. Byte-masked where a word also holds something else. The
 * texture identities at TMEM addresses are checked per rectangle instead (NrrHudHotIdent), and the
 * depth-clear test (colour image == depth image) per FILLRECT. */
GDX_COLD void Interp::NrrSnapInit() {
    NrrHud& N = *mNrr;
    N.nSnap = 0;
    const auto& tl = mRdp.texture_to_load;
    NrrSnapAdd(&tl.fmt, 1);
    NrrSnapAdd(&tl.siz, 1);
    NrrSnapAdd(&tl.width, sizeof(tl.width));
    NrrSnapAdd(&tl.cmd, sizeof(tl.cmd));
    NrrSnapAdd(&mRdp.texture_tile[0], sizeof(mRdp.texture_tile));
    NrrSnapAdd(&mRdp.first_tile_index, 1);
    NrrSnapAdd(&mRdp.other_mode_l, sizeof(mRdp.other_mode_l));
    NrrSnapAdd(&mRdp.other_mode_h, sizeof(mRdp.other_mode_h));
    NrrSnapAdd(&mRdp.combine_mode, sizeof(mRdp.combine_mode));
    NrrSnapAdd(&mRdp.grayscale, 1);
    NrrSnapAdd(&mRdp.blend_color.a, 1);
    NrrSnapAdd(&mRdp.scissor, sizeof(mRdp.scissor));
    NrrSnapAdd(&N.tlut[0], sizeof(N.tlut));
    for (uint32_t k = 1; k < N.nSnap; k++) { /* sorted by address (NrrSnapIndex) */
        for (uint32_t j = k; j > 0 && N.snapPtr[j - 1] > N.snapPtr[j]; j--) {
            const NrrWord32* pt = N.snapPtr[j];
            N.snapPtr[j] = N.snapPtr[j - 1];
            N.snapPtr[j - 1] = pt;
            const uint32_t mt = N.snapMask[j];
            N.snapMask[j] = N.snapMask[j - 1];
            N.snapMask[j - 1] = mt;
        }
    }
}

/* Bytes [p, p + len) into the snapshot list: the 32-bit words holding them, byte-masked. */
GDX_COLD void Interp::NrrSnapAdd(const void* p, uint32_t len) {
    NrrHud& N = *mNrr;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    for (uintptr_t byte = a; byte < a + len; byte++) {
        const NrrWord32* wp = reinterpret_cast<const NrrWord32*>(byte & ~static_cast<uintptr_t>(3));
        const uint32_t m = 0xFFu << (8u * (byte & 3u));
        uint32_t k = 0;
        while (k < N.nSnap && N.snapPtr[k] != wp) {
            k++;
        }
        if (k == N.nSnap) {
            if (N.nSnap == kSnapMax) {
                continue; /* cannot happen (static layout); a missing word would only split programs */
            }
            N.snapPtr[N.nSnap] = wp;
            N.snapMask[N.nSnap++] = 0;
        }
        N.snapMask[k] |= m;
    }
}

GDX_COLD void Interp::NrrSnapTake(uint32_t* dst) const {
    const NrrHud& N = *mNrr;
    for (uint32_t k = 0; k < N.nSnap; k++) {
        dst[k] = *N.snapPtr[k] & N.snapMask[k];
    }
}

/* The recorded program into the table: its skeleton, its entry snapshot, its ops (the pool full:
 * every program goes first). */
GDX_COLD void Interp::NrrProgStore(const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    uint32_t nDeps = 0;
    for (uint32_t k = 0; k < N.nSnap; k++) {
        nDeps += (N.recFull ? N.snapMask[k] : N.recRead[k]) != 0 ? 1u : 0u;
    }
    const uint32_t need = 3u * nDeps + N.recN;
    if (need > kPoolWords) {
        return;
    }
    if (N.poolUsed + need > kPoolWords || N.nProgs + 1u >= kProgs) {
        if (mNiMenuKeep && !mNiInSplit && NrrMenuKeepRefuse()) {
            mNiSplitWin.keepPool++;
            return; /* menus-s2b ni_menu_keep: not stored, every stored program stays */
        }
        if (GDX_UNLIKELY(mNiVolDiag > 0)) {
            mNiVolDiag--;
            GDXN_LOG("[nivol] progclear why=pool used=%u need=%u progs=%u seq=%u", N.poolUsed, need, N.nProgs, N.seq);
        }
        NrrProgClear();
    }
    const uint32_t h = NrrHudHotHash(w, n);
    const uint32_t slot0 = NrrProgSlot(h);
    NrrProg* g = &N.progs[slot0];
    for (uint32_t p = 0; p < kProgProbe; p++) {
        NrrProg& e = N.progs[(slot0 + p) & (kProgs - 1u)];
        if (e.n == 0) {
            g = &e;
            break;
        }
    }
    if (g->n == 0) {
        N.nProgs++;
    }
    uint32_t* d = N.pool + N.poolUsed;
    g->deps = N.poolUsed;
    g->nDeps = nDeps;
    for (uint32_t k = 0; k < N.nSnap; k++) {
        const uint32_t m = N.recFull ? N.snapMask[k] : N.recRead[k];
        if (m != 0) {
            *d++ = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(N.snapPtr[k]));
            *d++ = m;
            *d++ = N.recSnap[k] & m;
        }
    }
    g->ops = g->deps + 3u * nDeps;
    memcpy(N.pool + g->ops, N.rec, N.recN * sizeof(uint32_t));
    g->opWords = N.recN;
    g->hash = h;
    g->n = n;
    g->rr = 0; /* n60hudx nrr_hud_rr: the rectangles a record would hold, or no replay */
    g->rrInfo = 0;
    for (uint32_t k = 0; k < N.recN;) {
        const uint32_t kind = N.rec[k] & 7u;
        const uint32_t* pay = N.rec + k + 4;
        if (kind == kOpRt || kind == kOpFl) {
            g->rrInfo = (g->rrInfo & ~0xFFFFu) | (((g->rrInfo & 0xFFFFu) + 1u) & 0xFFFFu);
        } else if (kind == kOpEx) {
            g->rrInfo |= kRrNoReplay;
        }
        k += 4u + (kind == kOpSt ? 3u * ((N.rec[k] >> 4) & 15u)
                   : kind == kOpLd ? kLdWords
                   : kind == kOpRt ? kRtWords
                   : kind == kOpFl ? 1u
                   : kind == kOpCol ? 3u
                   : kind == kOpSl ? (pay[3] >> 16)
                                   : 0u);
    }
    N.poolUsed += need;
    N.w.progStores++;
}


/* ---- n60hudx [native] nrr_hud_rr: capture, record, verify (cold) ------------------------------ */

void Interp::NrrHudRrSet(int bits) {
    mNrrRr = bits & 7;
    if ((mNrrRr & 3) == 0) {
        mNrrRr = 0;
    }
}

/* The records go (the arena is reused); every program forgets its record. */
GDX_COLD void Interp::NrrRrReset() {
    NrrHud& N = *mNrr;
    N.rrUsed = 0;
    for (uint32_t k = 0; k < kProgs; k++) {
        N.progs[k].rr = 0;
    }
}

/* A program without a record: capture its execution if it may get one (lever bits, its rectangles, no
 * op a replay cannot carry, not after three records that missed -- then every 32nd task). */
GDX_COLD void Interp::NrrRrArm(const NrrProg& p) {
    NrrHud& N = *mNrr;
    const uint32_t rects = p.rrInfo & 0xFFFFu;
    const uint32_t misses = p.rrInfo >> kRrMissShift;
    if ((p.rrInfo & kRrSeen) == 0) {
        const_cast<NrrProg&>(p).rrInfo |= kRrSeen; /* a program used once is not recorded yet */
        return;
    }
    if ((p.rrInfo & kRrNoReplay) == 0 && rects != 0 && rects <= kRrMaxRects &&
        rects >= ((mNrrRr & 2) != 0 ? 1u : kRrBigRects) && (misses < 3u || (N.seq & 31u) == 0) &&
        N.rrCapTask < kRrCapsPerTask) {
        N.rrCapTask++;
        NrrRrCapBegin(p, false);
    }
}

/* A record that does not fit the run (recorded again), or verify mode (the program executes and its
 * output is compared with the record). */
GDX_COLD void Interp::NrrRrGateOut(const NrrProg& p) {
    NrrHud& N = *mNrr;
    const uint32_t* r = N.rrPool + (p.rr - 1u);
    uint32_t snap[kRrSnap];
    NrrHudHotRrSnap(snap);
    if ((mNrrRr & 4) != 0 && r[0] == p.n && r[1] == N.memoEpoch && r[2] == NrrStateGeneration() &&
        r[3] == mNrrTexEpoch && memcmp(snap, r + 6, sizeof(snap)) == 0 &&
        memcmp(N.runWords, r + kRrHead, p.n * sizeof(GdxHudWord)) == 0) {
        NrrRrCapBegin(p, true);
        return;
    }
    N.w.rrGateMiss++;
    NrrProg& m = const_cast<NrrProg&>(p);
    m.rr = 0;
    if ((m.rrInfo >> kRrMissShift) < 0x7FFFu) {
        m.rrInfo += 1u << kRrMissShift;
    }
    NrrRrArm(p);
}

GDX_COLD void Interp::NrrRrCapBegin(const NrrProg& p, bool verify) {
    NrrHud& N = *mNrr;
    (void)p;
    N.rrCap = true;
    N.rrCapBad = false;
    N.rrCapVfy = verify;
    N.rrCapN = 0;
    N.rrCapEpoch[0] = N.memoEpoch;
    N.rrCapEpoch[1] = NrrStateGeneration();
    N.rrCapEpoch[2] = mNrrTexEpoch;
    N.rrCapEpoch[3] = N.progEpoch;
    NrrHudHotRrSnap(N.rrCapSnap);
}

/* A program rectangle's output while its run is captured (p nullptr: an empty FILLRECT). */
GDX_COLD void Interp::NrrRrCapture(const uint32_t* op, const NrrBlock* b, const float* p) {
    NrrHud& N = *mNrr;
    if (N.rrCapN >= kRrMaxRects || op == nullptr || b->coldColour != 0 || N.xFull) {
        N.rrCapBad = true; /* not recorded: a block whose vertices read more than the record checks */
        return;
    }
    const uint32_t k = N.rrCapN++;
    N.rrCapOp[k] = op;
    N.rrCapBlock[k] = b;
    N.rrCapEmpty[k] = p == nullptr ? 1 : 0;
    if (p != nullptr) {
        memcpy(N.rrCapF[k], p, sizeof(N.rrCapF[k]));
    }
}

GDX_COLD void Interp::NrrRrCapEnd(const NrrProg& p, const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    N.rrCap = false;
    const bool clean = !N.rrCapBad && p.n == n && N.progEpoch == N.rrCapEpoch[3] && N.memoEpoch == N.rrCapEpoch[0] &&
                       NrrStateGeneration() == N.rrCapEpoch[1] && mNrrTexEpoch == N.rrCapEpoch[2];
    if (N.rrCapVfy) {
        if (clean) {
            NrrRrVerify(p, n);
        }
        return;
    }
    if (!clean || !NrrRrBuild(p, w, n)) {
        N.w.rrCapFails++;
        NrrProg& m = const_cast<NrrProg&>(p);
        if (p.n == n && (m.rrInfo >> kRrMissShift) < 0x7FFFu) {
            m.rrInfo += 1u << kRrMissShift;
        }
    }
}

/* The record of the program's execution just captured: the head, the run's words, the replay ops
 * (see the section comment), the rectangles' vertices. false: the program holds an op a replay does
 * not carry, or the capture does not match its rectangles. */
GDX_COLD bool Interp::NrrRrBuild(const NrrProg& p, const GdxHudWord* w, uint32_t n) {
    NrrHud& N = *mNrr;
    uint32_t* const buf = N.rrBuf;
    const uint32_t cap = kRrPoolWords / 4;
    uint32_t q = 0;     /* ops written */
    uint32_t blk = 0;   /* the open NS block's header index + 1, 0 = none (its groups follow it) */
    uint32_t ns = 0;    /* the open merged group's header index + 1, 0 = none */
    uint32_t rect = 0;  /* captured rectangles consumed */
    bool loaded = false; /* a load ran: the texture flag is set from here on (an arm is a no-op) */
    const uint32_t* op = N.pool + p.ops;
    const uint32_t* const end = op + p.opWords;
    /* one masked store into the open NS group (merged with an earlier store to its word) */
    auto store = [&](uint32_t dst, uint32_t mask, uint32_t val) -> bool {
        if (ns != 0) {
            const uint32_t h = ns - 1;
            const uint32_t cnt = (buf[h] >> 4) & 15u;
            for (uint32_t e = 0; e < cnt; e++) {
                if (buf[h + 1 + 3 * e] == dst) {
                    buf[h + 2 + 3 * e] |= mask;
                    buf[h + 3 + 3 * e] = (buf[h + 3 + 3 * e] & ~mask) | val;
                    return true;
                }
            }
            if (cnt == 15u) {
                ns = 0;
            }
        }
        if (q + 5 > cap) {
            return false;
        }
        if (blk == 0) {
            buf[q] = kRrNs;
            blk = ++q;
        }
        if (ns == 0) {
            buf[q] = kOpSt;
            ns = ++q;
        }
        buf[q++] = dst;
        buf[q++] = mask;
        buf[q++] = val;
        buf[ns - 1] += 1u << 4;
        return true;
    };
    while (op < end) {
        const uint32_t hdr = op[0];
        const uint32_t kind = hdr & 7u;
        const uint32_t i = hdr >> 16;
        const uint32_t* pay = op + 4;
        if (q + 8 > cap) {
            return false;
        }
        if (kind == kOpSt) {
            const uint32_t cnt = (hdr >> 4) & 15u;
            if ((hdr & kOpArm) != 0 && !loaded) {
                /* an arm before any load decides the texture flag: the group as it is, in order */
                ns = 0;
                if (blk == 0) {
                    buf[q] = kRrNs;
                    blk = ++q;
                }
                buf[q++] = kOpSt | (cnt << 4) | kOpArm;
                for (uint32_t k = 0; k < 3 * cnt; k++) {
                    buf[q++] = pay[k];
                }
            } else {
                for (uint32_t k = 0; k < cnt; k++) {
                    if (!store(pay[3 * k], pay[3 * k + 1], pay[3 * k + 2])) {
                        return false;
                    }
                }
            }
            op = pay + 3 * cnt;
        } else if (kind == kOpCol) {
            /* the colour register = the word's w1 in memory order; prim's lod fraction its w0's low byte
             * (nrr_hud_pd: the prim depth = w1's 15-bit z) */
            if ((hdr & kOpColPd) != 0) {
                if (!store(pay[0], 0xFFFFu << pay[2], ((w[i].w1 >> 16) & 0x7FFFu) << pay[2])) {
                    return false;
                }
            } else if (!store(pay[0], 0xFFFFFFFFu, __builtin_bswap32(w[i].w1))) {
                return false;
            }
            if (pay[1] != 0 && !store(pay[1], 0xFFu << pay[2], (w[i].w0 & 0xFFu) << pay[2])) {
                return false;
            }
            op = pay + 3;
        } else if (kind == kOpLd || kind == kOpSl) {
            ns = 0;
            if (blk != 0) {
                buf[blk - 1] |= (q - blk) << 16; /* the NS block's words */
                blk = 0;
            }
            buf[q++] = (kind == kOpLd ? kRrLd : kRrSl) | (i << 16);
            buf[q++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(op));
            loaded = loaded || kind == kOpLd;
            op = pay + (kind == kOpLd ? kLdWords : (pay[3] >> 16));
        } else if (kind == kOpRt || kind == kOpFl) {
            ns = 0;
            if (blk != 0) {
                buf[blk - 1] |= (q - blk) << 16;
                blk = 0;
            }
            if (rect >= N.rrCapN || N.rrCapOp[rect] != pay || N.rrCapBlock[rect] != reinterpret_cast<const NrrBlock*>(pay[0]) ||
                (kind == kOpRt && N.rrCapEmpty[rect] != 0)) {
                return false;
            }
            buf[q++] = (kind == kOpRt ? (kRrRt | (C1(w[i].w1, 24, 3) << 8)) : (kRrFl | (N.rrCapEmpty[rect] != 0 ? 1u << 11 : 0u))) |
                       (i << 16);
            buf[q++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(op));
            buf[q++] = pay[0];
            rect++;
            op = pay + (kind == kOpRt ? kRtWords : 1u);
        } else {
            return false; /* EX: re-decided at runtime */
        }
    }
    if (blk != 0) {
        buf[blk - 1] |= (q - blk) << 16;
    }
    if (rect != N.rrCapN) {
        return false;
    }
    uint32_t floats = 0;
    for (uint32_t k = 0; k < N.rrCapN; k++) {
        floats += N.rrCapEmpty[k] != 0 ? 0u : kQuadFloats;
    }
    const uint32_t need = kRrHead + 4u * n + q + floats;
    if (need > kRrPoolWords) {
        return false;
    }
    if (N.rrUsed + need > kRrPoolWords) {
        NrrRrReset();
    }
    uint32_t* r = N.rrPool + N.rrUsed;
    r[0] = n;
    r[1] = N.rrCapEpoch[0];
    r[2] = N.rrCapEpoch[1];
    r[3] = N.rrCapEpoch[2];
    r[4] = q;
    r[5] = N.rrCapN;
    memcpy(r + 6, N.rrCapSnap, sizeof(N.rrCapSnap));
    memcpy(r + kRrHead, w, n * sizeof(GdxHudWord));
    memcpy(r + kRrHead + 4u * n, buf, q * sizeof(uint32_t));
    float* f = reinterpret_cast<float*>(r + kRrHead + 4u * n + q);
    for (uint32_t k = 0; k < N.rrCapN; k++) {
        if (N.rrCapEmpty[k] == 0) {
            memcpy(f, N.rrCapF[k], sizeof(N.rrCapF[k]));
            f += kQuadFloats;
        }
    }
    NrrProg& m = const_cast<NrrProg&>(p);
    m.rr = N.rrUsed + 1u;
    N.rrUsed += need;
    N.w.rrRecs++;
    return true;
}

/* Verify (lever bit 4): the program executed a run its record would have replayed; its rectangles'
 * vertices against the record's, bit for bit, and its register words against the record's NS
 * stores (each word's last store). */
GDX_COLD void Interp::NrrRrVerify(const NrrProg& p, uint32_t n) {
    NrrHud& N = *mNrr;
    if (p.rr == 0) {
        return;
    }
    const uint32_t* r = N.rrPool + (p.rr - 1u);
    const uint32_t* const q0 = r + kRrHead + 4u * n;
    const uint32_t* const qe = q0 + r[4];
    const float* f = reinterpret_cast<const float*>(qe);
    bool bad = r[5] != N.rrCapN;
    bool sl = false;
    uint32_t rect = 0;
    /* the stores of every NS block, in order (dst, mask, val), for the register check */
    static uint32_t st[3 * 2048];
    uint32_t nst = 0;
    for (const uint32_t* q = q0; q < qe && !bad;) {
        const uint32_t kind = q[0] & 7u;
        if (kind == kRrNs) {
            const uint32_t* g = q + 1;
            const uint32_t* const ge = g + (q[0] >> 16);
            while (g < ge) {
                const uint32_t cnt = (g[0] >> 4) & 15u;
                for (uint32_t k = 0; k < cnt && nst < 2048; k++, nst++) {
                    st[3 * nst] = g[1 + 3 * k];
                    st[3 * nst + 1] = g[2 + 3 * k];
                    st[3 * nst + 2] = g[3 + 3 * k];
                }
                g += 1 + 3 * cnt;
            }
            q = ge;
        } else if (kind == kRrLd || kind == kRrSl) {
            sl = sl || kind == kRrSl;
            q += 2;
        } else {
            const bool empty = kind == kRrFl && (q[0] & (1u << 11)) != 0;
            if (rect >= N.rrCapN || empty != (N.rrCapEmpty[rect] != 0) ||
                (!empty && memcmp(f, N.rrCapF[rect], sizeof(N.rrCapF[rect])) != 0)) {
                bad = true;
            }
            if (!empty) {
                f += kQuadFloats;
            }
            rect++;
            q += 3;
        }
    }
    /* each NS-written bit's value at the end, against the record's last store to it (a record with an
     * SL op is checked on its vertices only: the list's own stores are not in its NS blocks) */
    for (uint32_t k = 0; k < nst && !bad && !sl; k++) {
        uint32_t mask = st[3 * k + 1];
        for (uint32_t j = k + 1; j < nst; j++) {
            if (st[3 * j] == st[3 * k]) {
                mask &= ~st[3 * j + 1];
            }
        }
        if (((*reinterpret_cast<const NrrWord32*>(st[3 * k]) ^ st[3 * k + 2]) & mask) != 0) {
            bad = true;
        }
    }
    if (bad) {
        N.w.rrVfyBad++;
        if (N.vfyLogs < 16) {
            N.vfyLogs++;
            GDXN_LOG("[nrrhud] rr vfy MISMATCH n=%lu rects=%lu/%lu at %lu", static_cast<unsigned long>(n),
                     static_cast<unsigned long>(N.rrCapN), static_cast<unsigned long>(r[5]), static_cast<unsigned long>(rect));
        }
    } else {
        N.w.rrVfyOk++;
    }
}

/* gdx_rdram_mode_reset's cache clear: the sources a pending load names may already belong to the
 * next mode (the arena rewound), so nothing pending is materialised -- dropped. The core's state
 * table follows (its blocks name texture ids the clear just retired). */
GDX_COLD void Interp::NrrHudClear() {
    if (mNrr == nullptr) {
        return;
    }
    NrrHud& N = *mNrr;
    if (GDX_UNLIKELY(mNiVolDiag > 0)) {
        mNiVolDiag--;
        GDXN_LOG("[nivol] memoclear why=modereset seq=%u", N.seq);
    }
    NrrMemoClear();
    N.nPend = 0;
    memset(N.slots, 0, sizeof(N.slots));
    N.tlut[0] = N.tlut[1] = 0;
    mNrrTmemLive = false;
    if (gdx3ds_nrr_level(NRR_CAT_HUD) != 0) {
        gdx3ds_nrr_state_flush();
    }
}

/* The task's end: what the HUD loaded virtually is put into the interpreter's TMEM now (the HUD is
 * the frame's last section, so this is what the next task's first reader would otherwise settle),
 * and the freshness sequence moves. */
void Interp::NrrHudTaskEnd() {
    mNiMenuOrd = 0; /* nimenu: run ordinals restart per task (gdx_nrr_menu.inc) */
    if (mNrr != nullptr) {
        if (mNrr->nPend != 0) {
            NrrMaterializeAll();
        }
        mNrr->memoTaskRects = 0;
        mNrr->rrCapTask = 0;
        mNrrRunInTask = 0;
        if (GDX_UNLIKELY(mNrrDepDiag > 0)) {
            NrrDepDiagTask();
        }
        sNrrHudPdOn = gdx3ds_cpucut_on(sNrrHudPd, GDX3DS_CPUCUT_BIT_HUDPD) != 0; /* o3ds-cpucuts nrr_hud_pd */
        mNrr->seq++;
    }
}

/* [debug] nrr_hud_runclock (n60hudx, diagnostic): per run position in the task, the ticks, tasks, words
 * and a signature of the run's opcodes (the window's last), for the [nrrrun] line. */
namespace {
constexpr uint32_t kRunClk = 40;
struct RunClk {
    uint64_t ticks;
    uint32_t n, words, ops;
};
RunClk sRunClk[kRunClk];
} // namespace

GDX_COLD void Interp::NrrRunClockNote(uint32_t runInTask, const void* runWords, uint32_t n, uint64_t ticks) {
    if (runInTask >= kRunClk) {
        runInTask = kRunClk - 1;
    }
    RunClk& r = sRunClk[runInTask];
    r.ticks += ticks;
    r.n++;
    r.words += n;
    const GdxHudWord* w = static_cast<const GdxHudWord*>(runWords);
    r.ops = (Opcode(w[0].w0) << 24) | (n > 1 ? Opcode(w[1].w0) << 16 : 0u) | (n > 2 ? Opcode(w[2].w0) << 8 : 0u) |
            (n > 3 ? Opcode(w[n - 1].w0) : 0u);
}

/* us per task of a clocked phase, one clock read (354 ticks, hudsky §1) taken off each interval. */
static double NrrPhUs(uint64_t ticks, uint32_t n, double inv) {
    const double t = static_cast<double>(ticks) - 354.0 * static_cast<double>(n);
    return (t > 0.0 ? t : 0.0) * inv * 1000.0 / 268111.856;
}

/* [nrrhud] per rendered task: runs, words, rects (fast = from a state block), the fallback arms
 * (stock rects + interp words: 0 in a steady race), virtual / real loads, materialisations
 * (settles, loads they ran, partial overwrites, stale reads), core draws, blocks built /
 * self-check failures / stock keys / memo clears; with nrr_hud_clock the run time and the
 * per-class split. */
GDX_COLD int Interp::NrrHudFormat(char* buf, size_t cap, uint32_t tasks) {
    if (mNrr == nullptr || cap == 0 || tasks == 0 || mNrrHud == 0) {
        return 0;
    }
    NrrHud& N = *mNrr;
    const double inv = 1.0 / static_cast<double>(tasks);
    int n = snprintf(buf, cap,
                     "[nrrhud] tasks=%u mode=%d runs=%.2f words=%.1f fallback=%.2f "
                     "(stock=%.2f interp=%.2f) prog dec=%.2f (nohash=%.2f deps=%.2f) ex=%.2f abandon=%.2f miss=%.2f stores=%u "
                     "recfail=%u clears=%u n=%u pool=%u | "
                     "reg cold=%.2f builds=%u n=%u | loads r=%.2f | "
                     "mat=%.2f matloads=%.2f partial=%u staleread=%u | draws=%.2f aux=%.2f blocks=%u builds=%u "
                     "selfbad=%u stockkeys=%u clears=%u own=%u ownfull=%u coreflush=%u | ses fallback=%u selfbad=%u",
                     static_cast<unsigned>(tasks), mNrrHud, N.w.runs * inv, N.w.words * inv,
                     (N.w.stockRects + N.w.interp) * inv, N.w.stockRects * inv, N.w.interp * inv, N.w.decRuns * inv,
                     N.w.progNoHash * inv, N.w.progDepMiss * inv, N.w.progEx * inv, N.w.progAbandons * inv,
                     N.w.progMiss * inv, N.w.progStores, N.w.recFails, N.w.progClears, N.nProgs, N.poolUsed,
                     N.w.regCold * inv, N.w.regBuilds, N.nRegs, N.w.rloads * inv,
                     N.w.mats * inv, N.w.matLoads * inv, N.w.partial, N.w.staleRead, N.w.draws * inv,
                     N.w.auxCold * inv, N.nBlocks, N.w.builds, N.w.selfBad, N.w.stockKeys, N.w.clears, N.nOwn,
                     N.w.ownFull, N.w.coreFlushes, N.sesFallback, N.sesSelfBad);
    if (mNrrLate != 0 && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n),
                      " | late=%d flush=%.2f scvar hit=%.2f build=%.2f table=%.2f promote=%u pitchmiss=%.2f", mNrrLate,
                      N.w.lateFlushes * inv, N.w.scVarHits * inv, N.w.scVarBuilds * inv, N.w.scVarTable * inv, N.w.scVarPromotes,
                      N.w.pitchMiss * inv);
    }
    if (mNrrRunClock != 0 && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | runclk");
        for (uint32_t k = 0; k < kRunClk && n > 0 && static_cast<size_t>(n) + 48 < cap; k++) {
            const RunClk& r = sRunClk[k];
            if (r.n != 0) {
                n += snprintf(buf + n, cap - static_cast<size_t>(n), " %lu:%.0fw/%.1fus/%08lx", static_cast<unsigned long>(k),
                              static_cast<double>(r.words) / r.n, NrrPhUs(r.ticks, r.n, inv), static_cast<unsigned long>(r.ops));
            }
        }
        memset(sRunClk, 0, sizeof(sRunClk));
    }
    if (mNrrRr != 0 && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n),
                      " | rr=%d hit=%.2f rects=%.2f gmiss=%.2f resume=%.2f rec=%u capfail=%u used=%u vfy ok=%u bad=%u", mNrrRr,
                      N.w.rrHits * inv, N.w.rrRects * inv, N.w.rrGateMiss * inv, N.w.rrResume * inv, N.w.rrRecs,
                      N.w.rrCapFails, N.rrUsed, N.w.rrVfyOk, N.w.rrVfyBad);
    }
    if (mNrrKept != 0 && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | kept sl=%.2f miss=%.2f dec=%.2f walk=%.2f build=%u",
                      N.w.slHits * inv, N.w.slMiss * inv, N.w.slDecs * inv, N.w.slWalks * inv, N.w.slBuilds);
    }
    if (N.memo != nullptr && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n), " | memo rects=%.2f hit=%.2f miss=%.2f skip=%.2f",
                      N.w.memoRects * inv, N.w.memoHits * inv, N.w.memoMisses * inv, N.w.memoSkips * inv);
    }
    if (mNrrHud == 2 && n > 0 && static_cast<size_t>(n) < cap) {
        const NrrHud::Vfy& v = N.vfy;
        n += snprintf(buf + n, cap - static_cast<size_t>(n),
                      " | vfy prog=%u reg ok=%u bad=%u new=%u ld ok=%u bad=%u rect ok=%u bad=%u new=%u skip=%u aux "
                      "ok=%u bad=%u loads=%u | ses_bad reg=%u ld=%u rect=%u aux=%u",
                      v.progRuns, v.regOk, v.regBad, v.regNew, v.ldOk, v.ldBad, v.rectOk, v.rectBad, v.rectNew,
                      v.rectSkip, v.auxOk, v.auxBad, v.loads, N.vfySes.regBad, N.vfySes.ldBad, N.vfySes.rectBad,
                      N.vfySes.auxBad);
        memset(&N.vfy, 0, sizeof(N.vfy));
    }
    if (N.clock != 0 && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(buf + n, cap - static_cast<size_t>(n),
                      " | run us=%.1f ph us reg=%.1f/%.1f load=%.1f/%.1f rect=%.1f/%.1f fill=%.1f/%.1f",
                      static_cast<double>(N.w.ticks) * inv * 1000.0 / 268111.856, NrrPhUs(N.w.ph[0], N.w.phN[0], inv),
                      N.w.phN[0] * inv, NrrPhUs(N.w.ph[1], N.w.phN[1], inv), N.w.phN[1] * inv,
                      NrrPhUs(N.w.ph[2], N.w.phN[2], inv), N.w.phN[2] * inv, NrrPhUs(N.w.ph[3], N.w.phN[3], inv),
                      N.w.phN[3] * inv);
    }
    memset(&N.w, 0, sizeof(N.w));
    return n;
}

static bool CensusSigSeen(uint32_t* sigs, uint32_t* nSigs, uint32_t sig) {
    for (uint32_t i = 0; i < *nSigs; i++) {
        if (sigs[i] == sig) {
            return true;
        }
    }
    if (*nSigs < kCensusSigs) {
        sigs[(*nSigs)++] = sig;
    }
    return false;
}

/* [native] nrr_hud_census=1: one [nrrhud] line per run of every kCensusEvery-th frame and per
 * run shape (opcode sequence) on first sight. Cold by construction. */
GDX_COLD void Interp::NrrCensusRun(const void* runWords, uint32_t n, uint32_t runIndex, uint32_t frame) {
    if (!NrrEnsure()) {
        return;
    }
    NrrHud& N = *mNrr;
    if (frame != N.lastFrame) {
        N.lastFrame = frame;
        N.runInFrame = 0;
    }
    const uint32_t ordinal = N.runInFrame++;
    const GdxHudWord* w = static_cast<const GdxHudWord*>(runWords);
    const uint32_t sig = RunSignature(w, n);
    const bool fresh = !CensusSigSeen(N.sigs, &N.nSigs, sig);
    if (!fresh && (frame % kCensusEvery) != 0) {
        return;
    }
    CenSim s;
    CenOut o;
    CenseInit(s, o, mRdp);
    for (uint32_t i = 0; i < n; i++) {
        CenseWord(s, o, w[i].w0, w[i].w1, w[i].w1hi);
    }
    char ops[400], st[200], tex[300];
    FormatOps(ops, sizeof(ops), o);
    FormatStates(st, sizeof(st), o);
    FormatTex(tex, sizeof(tex), o);
    GDXN_LOG("[nrrhud] census frame=%u run=%u ord=%u sig=%08lx new=%d words=%u rects=%u/%u pd=%u copy=%u "
             "box=%ld,%ld-%ld,%ld nst=%u nload=%u ops=%s st=%s tex=%s",
             static_cast<unsigned>(frame), static_cast<unsigned>(runIndex), static_cast<unsigned>(ordinal),
             static_cast<unsigned long>(sig), fresh ? 1 : 0, static_cast<unsigned>(n),
             static_cast<unsigned>(o.texRects), static_cast<unsigned>(o.fillRects),
             static_cast<unsigned>(o.primDepthRects), static_cast<unsigned>(o.copyRects),
             static_cast<long>(o.x0 / 4), static_cast<long>(o.y0 / 4), static_cast<long>(o.x1 / 4),
             static_cast<long>(o.y1 / 4), static_cast<unsigned>(o.nSt), static_cast<unsigned>(s.nLoad), ops, st, tex);
}

} // namespace gdxn

#include "gdx_nrr_menu.inc" /* nimenu [native] ni_menu: the 2D screen runner */
