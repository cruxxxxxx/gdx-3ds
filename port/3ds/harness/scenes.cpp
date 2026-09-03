/* port/3ds/harness/scenes.cpp — hand-built F3DEX2 display lists, one scene per
 * TODO(citra-verify) item in port/3ds/gfx/STATUS.md.
 *
 * This TU builds DLs with the decomp's PR/gbi.h writer macros (g* forms) and
 * includes NO libultraship headers: the two gbi header families define
 * colliding G_* macros, so the boundary to the interpreter is scenes.h's
 * opaque void*.
 *
 * Layout compatibility (checked at runtime by dl_tests_main.cpp against
 * kSceneGfxPacketSize): WITHOUT the PORT define, this gbi.h's Gfx is
 * { unsigned int w0; unsigned int w1; } — 8 bytes, w1 carries host pointers
 * casted to 32 bits. That is byte-identical to Fast::F3DGfx
 * ({ uintptr_t w0, w1 }) on the 32-bit 3DS, and the interpreter's f3dex2/RDP
 * handlers decode exactly these standard F3DEX2 encodings (SegAddr() passes
 * even-aligned host pointers straight through).
 *
 * Coordinate conventions used by every scene:
 *   - "window" = 320x240 (dl_tests_main.cpp inits the interpreter at native
 *     N64 resolution, so viewport/scissor map 1:1 and no widescreen scaling
 *     kicks in)
 *   - model space: x right in [-160,160], y UP in [-120,120], via an ortho
 *     projection matrix; z = 0 plane unless stated
 *   - N64 raster space (scissor coords): origin TOP-LEFT, y down
 */

#define _LANGUAGE_C_PLUS_PLUS /* ultratypes.h gate; the decomp build defines it too */
#include <PR/mbi.h> /* _SHIFTL, used by every g*() writer macro */
#include <PR/gbi.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "scenes.h"

/* linearAlloc lives in <3ds.h>, but libctru's u32/u64 typedefs (unsigned int
 * based) collide with ultratypes.h's (unsigned long based). Declare the one
 * function we need instead. */
extern "C" void* linearAlloc(size_t size);

const size_t kSceneGfxPacketSize = sizeof(Gfx);
static_assert(sizeof(Gfx) == 8, "non-PORT gbi.h Gfx must be the 8-byte packet");

/* ------------------------------------------------------------------------- */
/* Storage                                                                    */
/* ------------------------------------------------------------------------- */

enum { kDlCap = 256 };
enum { kTexW = 32, kTexH = 32 };

enum { kDualTexW = 32, kDualTexH = 64 }; /* scene 6: RGBA16, two stacked 32x32 views */

static Gfx* sDl;      /* rebuilt every SceneBuildDl() call */
static Vp* sVp;
static Mtx* sProj;    /* ortho: x/160, y/120, z/512 */
static Mtx* sIdent;
static Mtx* sRot[8];  /* RotZ(k * 45deg) */
static unsigned char* sTex; /* 32x32 RGBA32 arrow — linearAlloc, see scenes.h */
static unsigned char* sDualTex; /* 32x64 RGBA16 (arrow view / checker view) — linearAlloc */

static Vtx* sQuadCorners;  /* scene 0: 4 distinct-colour corners */
static Vtx* sTriangle;     /* scene 1 */
static Vtx* sTexQuad;      /* scene 2 */
static Vtx* sFullQuad;     /* scene 3 */
static Vtx* sDecalBase;    /* scene 4 */
static Vtx* sDecalTop;     /* scene 4 */
static Vtx* sCombineQuads; /* scene 5: 4 quads x 4 verts */
static Vtx* sTexel1Quads;  /* scene 6: 3 quads x 4 verts */
static Vtx* sMachineQuads; /* scene 7: 2 quads x 4 verts */
static Vtx* sFogQuad;      /* scene 8: depth-tilted full-width quad */

static void SetVtx(Vtx* v, short x, short y, short z, short s, short t,
                   unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    v->v.ob[0] = x;
    v->v.ob[1] = y;
    v->v.ob[2] = z;
    v->v.flag = 0;
    v->v.tc[0] = s;
    v->v.tc[1] = t;
    v->v.cn[0] = r;
    v->v.cn[1] = g;
    v->v.cn[2] = b;
    v->v.cn[3] = a;
}

/* guMtxF2L-equivalent: 16.16 fixed point, 8 packed int words then 8 packed
 * frac words — exactly the layout Interpreter::GfxSpMatrix() decodes. */
static void MtxFromFloat(Mtx* dst, const float mf[4][4]) {
    unsigned int* e = (unsigned int*)dst;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 2; j++) {
            int a = (int)(mf[i][2 * j] * 65536.0f);
            int b = (int)(mf[i][2 * j + 1] * 65536.0f);
            e[i * 2 + j] = ((unsigned int)a & 0xFFFF0000u) | (((unsigned int)b >> 16) & 0xFFFFu);
            e[8 + i * 2 + j] = (((unsigned int)a << 16) & 0xFFFF0000u) | ((unsigned int)b & 0xFFFFu);
        }
    }
}

static void BuildMatrices(void) {
    const float ident[4][4] = {
        { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }
    };
    MtxFromFloat(sIdent, ident);

    /* Row-vector ortho: x/160 -> NDC x, y/120 -> NDC y (+y up), z/512. */
    const float ortho[4][4] = {
        { 2.0f / 320.0f, 0, 0, 0 },
        { 0, 2.0f / 240.0f, 0, 0 },
        { 0, 0, -1.0f / 512.0f, 0 },
        { 0, 0, 0, 1 }
    };
    MtxFromFloat(sProj, ortho);

    /* Row-vector RotZ: v' = v*M rotates counterclockwise (x right, y up) for
     * positive angles. 8 steps of 45 degrees. */
    for (int k = 0; k < 8; k++) {
        const float th = (float)k * (float)(M_PI / 4.0);
        const float c = cosf(th), s = sinf(th);
        const float rot[4][4] = {
            { c, s, 0, 0 }, { -s, c, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }
        };
        MtxFromFloat(sRot[k], rot);
    }
}

/* 32x32 RGBA32 (byte order R,G,B,A), deliberately asymmetric on BOTH axes:
 *   rows 0-1            solid RED    (must render at the TOP)
 *   cols 0-1 (row 2+)   solid GREEN  (must render at the LEFT)
 *   white UP-pointing arrow on dark grey elsewhere                        */
static void BuildTexture(void) {
    for (int y = 0; y < kTexH; y++) {
        for (int x = 0; x < kTexW; x++) {
            unsigned char* p = sTex + 4 * (y * kTexW + x);
            unsigned char r = 60, g = 60, b = 60;
            if (y < 2) {
                r = 255; g = 0; b = 0;
            } else if (x < 2) {
                r = 0; g = 255; b = 0;
            } else {
                int white = 0;
                if (y >= 4 && y <= 15) { /* arrowhead: widens downwards from the apex */
                    const int hw = y - 3;
                    white = (x >= 16 - hw && x <= 15 + hw);
                } else if (y >= 16 && y <= 27) { /* shaft */
                    white = (x >= 14 && x <= 17);
                }
                if (white) {
                    r = 255; g = 255; b = 255;
                }
            }
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
    }
}

/* 32x64 RGBA16 (5551, big-endian byte pairs — the N64 memory layout the
 * interpreter's TMEM decode reads), two stacked 32-row views of ONE TMEM load:
 *   rows  0-31  view A: 2px RED top stripe, 2px GREEN left cols, white
 *               UP-arrow on dark grey (same design language as sTex)
 *   rows 32-55  view B: 8px BLUE/YELLOW checkerboard
 *   rows 56-63  MAGENTA — outside tile 1's 24-row window; visible magenta
 *               means the TEXEL1 tile extent/clamp is broken               */
static void BuildDualTexture(void) {
    for (int y = 0; y < kDualTexH; y++) {
        for (int x = 0; x < kDualTexW; x++) {
            unsigned char r, g, b;
            if (y < 32) {
                r = 60; g = 60; b = 60;
                if (y < 2) {
                    r = 255; g = 0; b = 0;
                } else if (x < 2) {
                    r = 0; g = 255; b = 0;
                } else {
                    int white = 0;
                    if (y >= 4 && y <= 15) {
                        const int hw = y - 3;
                        white = (x >= 16 - hw && x <= 15 + hw);
                    } else if (y >= 16 && y <= 27) {
                        white = (x >= 14 && x <= 17);
                    }
                    if (white) {
                        r = 255; g = 255; b = 255;
                    }
                }
            } else if (y < 56) {
                const int cell = ((x >> 3) + ((y - 32) >> 3)) & 1;
                if (cell) {
                    r = 255; g = 255; b = 0; /* yellow */
                } else {
                    r = 0; g = 64; b = 255;  /* blue */
                }
            } else {
                r = 255; g = 0; b = 255; /* magenta sentinel */
            }
            const unsigned p = ((unsigned)(r >> 3) << 11) | ((unsigned)(g >> 3) << 6) |
                               ((unsigned)(b >> 3) << 1) | 1u;
            unsigned char* d = sDualTex + 2 * (y * kDualTexW + x);
            d[0] = (unsigned char)(p >> 8);
            d[1] = (unsigned char)(p & 0xFF);
        }
    }
}

int ScenesInit(void) {
    sDl = (Gfx*)malloc(sizeof(Gfx) * kDlCap);
    sVp = (Vp*)malloc(sizeof(Vp));
    sProj = (Mtx*)malloc(sizeof(Mtx));
    sIdent = (Mtx*)malloc(sizeof(Mtx));
    for (int k = 0; k < 8; k++) {
        sRot[k] = (Mtx*)malloc(sizeof(Mtx));
    }
    sTex = (unsigned char*)linearAlloc(kTexW * kTexH * 4);
    sDualTex = (unsigned char*)linearAlloc(kDualTexW * kDualTexH * 2);
    sQuadCorners = (Vtx*)malloc(sizeof(Vtx) * 4);
    sTriangle = (Vtx*)malloc(sizeof(Vtx) * 3);
    sTexQuad = (Vtx*)malloc(sizeof(Vtx) * 4);
    sFullQuad = (Vtx*)malloc(sizeof(Vtx) * 4);
    sDecalBase = (Vtx*)malloc(sizeof(Vtx) * 4);
    sDecalTop = (Vtx*)malloc(sizeof(Vtx) * 4);
    sCombineQuads = (Vtx*)malloc(sizeof(Vtx) * 16);
    sTexel1Quads = (Vtx*)malloc(sizeof(Vtx) * 12);
    sMachineQuads = (Vtx*)malloc(sizeof(Vtx) * 8);
    sFogQuad = (Vtx*)malloc(sizeof(Vtx) * 4);
    if (!sDl || !sVp || !sProj || !sIdent || !sTex || !sDualTex || !sQuadCorners || !sTriangle ||
        !sTexQuad || !sFullQuad || !sDecalBase || !sDecalTop || !sCombineQuads || !sTexel1Quads ||
        !sMachineQuads || !sFogQuad) {
        return 0;
    }
    for (int k = 0; k < 8; k++) {
        if (!sRot[k]) {
            return 0;
        }
    }

    /* Full 320x240 viewport, 2-bit-fraction units, z scale/trans 511/0 to
     * match SpReset()'s defaults. */
    sVp->vp.vscale[0] = 160 * 4;
    sVp->vp.vscale[1] = 120 * 4;
    sVp->vp.vscale[2] = 511;
    sVp->vp.vscale[3] = 0;
    sVp->vp.vtrans[0] = 160 * 4;
    sVp->vp.vtrans[1] = 120 * 4;
    sVp->vp.vtrans[2] = 0;
    sVp->vp.vtrans[3] = 0;

    BuildMatrices();
    BuildTexture();
    BuildDualTexture();

    /* Scene 0 — corner colours. TL red / TR green / BL blue / BR white. */
    SetVtx(&sQuadCorners[0], -80, 60, 0, 0, 0, 255, 0, 0, 255);
    SetVtx(&sQuadCorners[1], 80, 60, 0, 0, 0, 0, 255, 0, 255);
    SetVtx(&sQuadCorners[2], -80, -60, 0, 0, 0, 0, 0, 255, 255);
    SetVtx(&sQuadCorners[3], 80, -60, 0, 0, 0, 255, 255, 255, 255);

    /* Scene 1 — gouraud triangle, red apex UP. */
    SetVtx(&sTriangle[0], 0, 80, 0, 0, 0, 255, 0, 0, 255);
    SetVtx(&sTriangle[1], -70, -40, 0, 0, 0, 0, 255, 0, 255);
    SetVtx(&sTriangle[2], 70, -40, 0, 0, 0, 0, 0, 255, 255);

    /* Scene 2 — 128x128 textured quad; t=0 on the TOP edge. tc is s10.5. */
    SetVtx(&sTexQuad[0], -64, 64, 0, 0, 0, 255, 255, 255, 255);
    SetVtx(&sTexQuad[1], 64, 64, 0, 31 << 5, 0, 255, 255, 255, 255);
    SetVtx(&sTexQuad[2], -64, -64, 0, 0, 31 << 5, 255, 255, 255, 255);
    SetVtx(&sTexQuad[3], 64, -64, 0, 31 << 5, 31 << 5, 255, 255, 255, 255);

    /* Scene 3 — full-screen quad (scissored to the top-left quadrant). */
    SetVtx(&sFullQuad[0], -160, 120, 0, 0, 0, 255, 255, 0, 255);
    SetVtx(&sFullQuad[1], 160, 120, 0, 0, 0, 255, 255, 0, 255);
    SetVtx(&sFullQuad[2], -160, -120, 0, 0, 0, 255, 255, 0, 255);
    SetVtx(&sFullQuad[3], 160, -120, 0, 0, 0, 255, 255, 0, 255);

    /* Scene 4 — two coplanar quads at z=0: blue 200x140 base, red 100x70 decal. */
    SetVtx(&sDecalBase[0], -100, 70, 0, 0, 0, 0, 0, 255, 255);
    SetVtx(&sDecalBase[1], 100, 70, 0, 0, 0, 0, 0, 255, 255);
    SetVtx(&sDecalBase[2], -100, -70, 0, 0, 0, 0, 0, 255, 255);
    SetVtx(&sDecalBase[3], 100, -70, 0, 0, 0, 0, 0, 255, 255);
    SetVtx(&sDecalTop[0], -50, 35, 0, 0, 0, 255, 0, 0, 255);
    SetVtx(&sDecalTop[1], 50, 35, 0, 0, 0, 255, 0, 0, 255);
    SetVtx(&sDecalTop[2], -50, -35, 0, 0, 0, 255, 0, 0, 255);
    SetVtx(&sDecalTop[3], 50, -35, 0, 0, 0, 255, 0, 0, 255);

    /* Scene 5 — four 64x56 quads, centres x = -120,-40,+40,+120; mid-grey
     * shade so MULTIPLY_ADD's tex*shade term is visibly darkened. */
    for (int q = 0; q < 4; q++) {
        const short cx = (short)(-120 + 80 * q);
        Vtx* v = &sCombineQuads[q * 4];
        SetVtx(&v[0], cx - 32, 28, 0, 0, 0, 128, 128, 128, 255);
        SetVtx(&v[1], cx + 32, 28, 0, 31 << 5, 0, 128, 128, 128, 255);
        SetVtx(&v[2], cx - 32, -28, 0, 0, 31 << 5, 128, 128, 128, 255);
        SetVtx(&v[3], cx + 32, -28, 0, 31 << 5, 31 << 5, 128, 128, 128, 255);
    }

    /* Scene 6 — three 80x80 quads at x = -110, 0, +110.
     * Q1 samples TEXEL0 (tile 0, 32 rows), Q2 samples TEXEL1 (tile 1, 24-row
     * window), Q3 lerps T0->T1 by a horizontal shade ramp (white -> black). */
    for (int q = 0; q < 3; q++) {
        const short cx = (short)(-110 + 110 * q);
        const short tMax = (short)(q == 0 ? (31 << 5) : (23 << 5));
        const unsigned char shadeR = (unsigned char)(q == 2 ? 0 : 255); /* right column shade */
        Vtx* v = &sTexel1Quads[q * 4];
        SetVtx(&v[0], cx - 40, 40, 0, 0, 0, 255, 255, 255, 255);
        SetVtx(&v[1], cx + 40, 40, 0, 31 << 5, 0, shadeR, shadeR, shadeR, 255);
        SetVtx(&v[2], cx - 40, -40, 0, 0, tMax, 255, 255, 255, 255);
        SetVtx(&v[3], cx + 40, -40, 0, 31 << 5, tMax, shadeR, shadeR, shadeR, 255);
    }

    /* Scene 7 — two 100x90 quads at x = -80, +80: census #16 machine material.
     * Left quad shade = white (pure cycle-1 lerp), right quad shade ramps
     * white -> dark so the cycle-2 shade-modulate is visible. */
    for (int q = 0; q < 2; q++) {
        const short cx = (short)(-80 + 160 * q);
        const unsigned char shadeR = (unsigned char)(q == 0 ? 255 : 40);
        Vtx* v = &sMachineQuads[q * 4];
        SetVtx(&v[0], cx - 50, 45, 0, 0, 0, 255, 255, 255, 255);
        SetVtx(&v[1], cx + 50, 45, 0, 31 << 5, 0, shadeR, shadeR, shadeR, 255);
        SetVtx(&v[2], cx - 50, -45, 0, 0, 31 << 5, 255, 255, 255, 255);
        SetVtx(&v[3], cx + 50, -45, 0, 31 << 5, 31 << 5, shadeR, shadeR, shadeR, 255);
    }

    /* Scene 8 — full-width RED quad tilted in depth: left edge near (z=+400,
     * z/w = -0.78), right edge far (z=-400, z/w = +0.78). */
    SetVtx(&sFogQuad[0], -160, 100, 400, 0, 0, 255, 0, 0, 255);
    SetVtx(&sFogQuad[1], 160, 100, -400, 0, 0, 255, 0, 0, 255);
    SetVtx(&sFogQuad[2], -160, -100, 400, 0, 0, 255, 0, 0, 255);
    SetVtx(&sFogQuad[3], 160, -100, -400, 0, 0, 255, 0, 0, 255);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* DL building                                                                */
/* ------------------------------------------------------------------------- */

/* Full state reset every scene runs first: known othermode word (1-cycle,
 * point sampling, no dither/LUT), full-screen scissor + viewport, geometry
 * mode down to smooth shading, ortho projection, identity modelview. */
static Gfx* Prologue(Gfx* g, unsigned int extraGeom, unsigned int renderMode) {
    gDPPipeSync(g++);
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
    gSPViewport(g++, sVp);
    gSPClearGeometryMode(g++, 0x00FFFFFF);
    gSPSetGeometryMode(g++, G_SHADE | G_SHADING_SMOOTH | extraGeom);
    gDPSetOtherMode(g++,
                    G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE |
                        G_TL_TILE | G_TD_CLAMP | G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PIXEL | renderMode);
    gSPMatrix(g++, sProj, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(g++, sIdent, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    return g;
}

static Gfx* Quad(Gfx* g, Vtx* v) {
    gSPVertex(g++, v, 4, 0);
    gSP2Triangles(g++, 0, 2, 1, 0, 1, 2, 3, 0);
    return g;
}

static Gfx* LoadArrowTexture(Gfx* g) {
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPLoadTextureBlock(g++, sTex, G_IM_FMT_RGBA, G_IM_SIZ_32b, kTexW, kTexH, 0,
                        G_TX_CLAMP, G_TX_CLAMP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    return g;
}

static Gfx* BuildStrip(Gfx* g) {
    g = Prologue(g, 0, G_RM_OPA_SURF | G_RM_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    g = Quad(g, sQuadCorners);
    return g;
}

static Gfx* BuildRotate(Gfx* g, unsigned frame) {
    g = Prologue(g, 0, G_RM_OPA_SURF | G_RM_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPMatrix(g++, sRot[(frame >> 4) & 7], G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPVertex(g++, sTriangle, 3, 0);
    gSP1Triangle(g++, 0, 1, 2, 0);
    return g;
}

static Gfx* BuildTexture(Gfx* g) {
    g = Prologue(g, 0, G_RM_OPA_SURF | G_RM_OPA_SURF2);
    g = LoadArrowTexture(g);
    gDPSetCombineMode(g++, G_CC_DECALRGBA, G_CC_DECALRGBA);
    g = Quad(g, sTexQuad);
    return g;
}

static Gfx* BuildScissor(Gfx* g) {
    g = Prologue(g, 0, G_RM_OPA_SURF | G_RM_OPA_SURF2);
    /* N64 raster coords, origin top-left: this is the TOP-LEFT quadrant. */
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 160, 120);
    gDPSetCombineMode(g++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(g++, 0, 0, 255, 255, 0, 255);
    g = Quad(g, sFullQuad);
    return g;
}

static Gfx* BuildDecal(Gfx* g) {
    g = Prologue(g, G_ZBUFFER, G_RM_ZB_OPA_SURF | G_RM_ZB_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    g = Quad(g, sDecalBase);
    gDPPipeSync(g++);
    gDPSetRenderMode(g++, G_RM_ZB_OPA_DECAL, G_RM_ZB_OPA_DECAL2);
    g = Quad(g, sDecalTop);
    return g;
}

static Gfx* BuildCombine(Gfx* g) {
    g = Prologue(g, 0, G_RM_OPA_SURF | G_RM_OPA_SURF2);
    g = LoadArrowTexture(g);
    gDPSetPrimColor(g++, 0, 0, 255, 128, 0, 255); /* orange */
    gDPSetEnvColor(g++, 0, 128, 255, 255);        /* azure  */

    /* Q1 REPLACE: raw texture. */
    gDPSetCombineMode(g++, G_CC_DECALRGBA, G_CC_DECALRGBA);
    g = Quad(g, &sCombineQuads[0]);

    /* Q2 MODULATE: texel * prim. */
    gDPPipeSync(g++);
    gDPSetCombineLERP(g++, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, 1,
                      TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, 1);
    g = Quad(g, &sCombineQuads[4]);

    /* Q3 INTERPOLATE: (texel - prim) * env + prim == per-channel lerp. */
    gDPPipeSync(g++);
    gDPSetCombineLERP(g++, TEXEL0, PRIMITIVE, ENVIRONMENT, PRIMITIVE, 0, 0, 0, 1,
                      TEXEL0, PRIMITIVE, ENVIRONMENT, PRIMITIVE, 0, 0, 0, 1);
    g = Quad(g, &sCombineQuads[8]);

    /* Q4 MULTIPLY_ADD: texel * shade + prim. */
    gDPPipeSync(g++);
    gDPSetCombineLERP(g++, TEXEL0, 0, SHADE, PRIMITIVE, 0, 0, 0, 1,
                      TEXEL0, 0, SHADE, PRIMITIVE, 0, 0, 0, 1);
    g = Quad(g, &sCombineQuads[12]);
    return g;
}

/* Same othermode H bits as Prologue() but 2-cycle — the mode every in-race
 * track/machine material runs under (combiner census 2c/2d). */
static Gfx* OtherMode2Cycle(Gfx* g, unsigned int renderMode) {
    gDPPipeSync(g++);
    gDPSetOtherMode(g++,
                    G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE |
                        G_TL_TILE | G_TD_CLAMP | G_TP_NONE | G_CYC_2CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PIXEL | renderMode);
    return g;
}

/* Scene 6 — the F-Zero X adjacent-tile TEXEL1 pattern (interpreter.cpp:3182-3188):
 * ONE TMEM load, render tile 0 and render tile (0+1) describing two different
 * views of it. Tile 1 sits at an interior TMEM word with a 24-row (non-pow2)
 * window so the backend's per-unit UV rescale is exercised too. */
static Gfx* BuildTexel1(Gfx* g) {
    g = Prologue(g, 0, G_RM_OPA_SURF | G_RM_OPA_SURF2);
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    /* One block load: the whole 32x64 RGBA16 image (4096 B = all of TMEM). */
    gDPLoadTextureBlock(g++, sDualTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, kDualTexW, kDualTexH, 0,
                        G_TX_CLAMP, G_TX_CLAMP, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
    /* Re-point the render tiles: tile 0 = rows 0-31 (view A) at TMEM 0; tile 1 =
     * a 32x24 window (view B) at TMEM word 256 (row 32 x 8 words/row). */
    gDPSetTile(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 0, G_TX_RENDERTILE, 0,
               G_TX_CLAMP, 5, G_TX_NOLOD, G_TX_CLAMP, 5, G_TX_NOLOD);
    gDPSetTileSize(g++, G_TX_RENDERTILE, 0, 0, 31 << 2, 31 << 2);
    gDPSetTile(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 256, 1, 0,
               G_TX_CLAMP, 5, G_TX_NOLOD, G_TX_CLAMP, 5, G_TX_NOLOD);
    gDPSetTileSize(g++, 1, 0, 0, 31 << 2, 23 << 2);

    /* Q1: TEXEL0 only (view A), 1-cycle reference. */
    gDPSetCombineMode(g++, G_CC_DECALRGBA, G_CC_DECALRGBA);
    g = Quad(g, &sTexel1Quads[0]);

    /* Q2: TEXEL1 only, 2-cycle (cycle 2 passes COMBINED through). */
    g = OtherMode2Cycle(g, G_RM_PASS | G_RM_OPA_SURF2);
    gDPSetCombineLERP(g++, 0, 0, 0, TEXEL1, 0, 0, 0, 1,
                      0, 0, 0, COMBINED, 0, 0, 0, COMBINED);
    g = Quad(g, &sTexel1Quads[4]);

    /* Q3: (T0 - T1) * SHADE + T1 — lerp between the two views by the shade ramp. */
    gDPPipeSync(g++);
    gDPSetCombineLERP(g++, TEXEL0, TEXEL1, SHADE, TEXEL1, 0, 0, 0, 1,
                      0, 0, 0, COMBINED, 0, 0, 0, COMBINED);
    g = Quad(g, &sTexel1Quads[8]);
    return g;
}

/* Scene 7 — census #16 (the 271-site custom-machine material): cycle 1
 * lerp(ENV -> PRIM by TEXEL0), cycle 2 multiplies by SHADE. Exercises the
 * 3-stage chain with the constant-spill prefix stage. */
static Gfx* BuildMachine(Gfx* g) {
    g = Prologue(g, 0, G_RM_OPA_SURF | G_RM_OPA_SURF2);
    g = LoadArrowTexture(g);
    g = OtherMode2Cycle(g, G_RM_PASS | G_RM_OPA_SURF2);
    gDPSetPrimColor(g++, 0, 0, 255, 40, 40, 255); /* PRIM red  (arrow-white texels)  */
    gDPSetEnvColor(g++, 0, 32, 255, 255);         /* ENV  blue (dark-grey texels)    */
    gDPSetCombineLERP(g++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, 0, 0, 0, ENVIRONMENT,
                      COMBINED, 0, SHADE, 0, 0, 0, 0, COMBINED);
    g = Quad(g, &sMachineQuads[0]);
    g = Quad(g, &sMachineQuads[4]);
    return g;
}

/* Scene 8 — fogged draw (PICA native fog unit): G_FOG vertex fog + the
 * G_RM_FOG_SHADE_A blender, red shade fading to the blue fog colour with depth. */
static Gfx* BuildFog(Gfx* g) {
    g = Prologue(g, G_FOG, G_RM_OPA_SURF | G_RM_OPA_SURF2);
    g = OtherMode2Cycle(g, G_RM_FOG_SHADE_A | G_RM_OPA_SURF2);
    gSPFogPosition(g++, 400, 900);
    gDPSetFogColor(g++, 40, 60, 255, 255); /* blue */
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_PASS2);
    g = Quad(g, sFogQuad);
    return g;
}

void* SceneBuildDl(int scene, unsigned frame) {
    Gfx* g = sDl;
    switch (scene) {
        case 0: g = BuildStrip(g); break;
        case 1: g = BuildRotate(g, frame); break;
        case 2: g = BuildTexture(g); break;
        case 3: g = BuildScissor(g); break;
        case 4: g = BuildDecal(g); break;
        case 5: g = BuildCombine(g); break;
        case 6: g = BuildTexel1(g); break;
        case 7: g = BuildMachine(g); break;
        case 8: g = BuildFog(g); break;
        default: break;
    }
    gDPFullSync(g++);
    gSPEndDisplayList(g++);
    return sDl;
}

/* ------------------------------------------------------------------------- */
/* Scene metadata (printed to the bottom-screen console)                      */
/* ------------------------------------------------------------------------- */

static const SceneInfo kScenes[kSceneCount] = {
    { "STRIP",
      "vertex order / winding + viewport origin",
      "Centred quad, smooth gradient:\n"
      "  RED top-left    GREEN top-right\n"
      "  BLUE bot-left   WHITE bot-right\n"
      "Red anywhere else = viewport/Y flip." },
    { "ROTATE",
      "fixup-matrix rotation sign",
      "Gouraud triangle, RED apex starts\n"
      "at the top and steps COUNTER-\n"
      "clockwise (apex moves LEFT first),\n"
      "45deg every ~16 frames." },
    { "TEXTURE",
      "texture row order + UV orientation",
      "White UP arrow on grey, 2px RED\n"
      "stripe on TOP edge, 2px GREEN\n"
      "stripe on LEFT edge.\n"
      "Red at bottom = row-order flip;\n"
      "green at right = S mirrored." },
    { "SCISSOR",
      "scissor rectangle origin",
      "YELLOW fills ONLY the top-left\n"
      "quadrant; rest stays black.\n"
      "Bottom-left = scissor Y origin\n"
      "flipped; top-right = axis swap." },
    { "DECAL",
      "decal zmode depth offset",
      "RED 100x70 rectangle cleanly on\n"
      "top of a BLUE 200x140 one (same\n"
      "z plane). Red missing = offset\n"
      "sign wrong; speckle = too small." },
    { "COMBINE",
      "TexEnv combiner mapping",
      "4 arrow quads, left to right:\n"
      " 1 REPLACE  raw texture\n"
      " 2 MODULATE tex*orange\n"
      " 3 LERP     orange->tex by azure\n"
      " 4 MULADD   tex*grey+orange\n"
      "Any 'unmapped combiner' stderr\n"
      "line = mapping regression." },
    { "TEXEL1",
      "2-cycle adjacent-tile TEXEL1 (unit 1 bind + UV1 scale)",
      "3 quads, left to right:\n"
      " 1 ARROW  (tile 0 view)\n"
      " 2 BLUE/YELLOW checker (tile 1)\n"
      " 3 arrow LEFT -> checker RIGHT\n"
      "Q2 arrow/grey = unit1 not bound;\n"
      "magenta = tile-1 extent broken;\n"
      "black bands = UV1 scale wrong." },
    { "MACHINE",
      "census #16 3-stage 2-cycle machine material",
      "2 arrow quads:\n"
      " L: BLUE body, RED arrow (lerp\n"
      "    ENV->PRIM by texel)\n"
      " R: same, darkening to the\n"
      "    right (x SHADE ramp)\n"
      "Flat grey/white = 2cyc chain\n"
      "broken; no darkening = stage-2\n"
      "shade modulate lost." },
    { "FOG",
      "PICA native fog LUT vs interpreter fog vertices",
      "Full-screen RED quad:\n"
      " pure red on the LEFT third,\n"
      " fading to BLUE at the right\n"
      " edge (left=near, right=far).\n"
      "Reversed gradient = fog LUT\n"
      "depth direction flipped; no\n"
      "blue = fog unit never enabled." },
};

const SceneInfo* SceneGetInfo(int scene) {
    if (scene < 0 || scene >= kSceneCount) {
        return &kScenes[0];
    }
    return &kScenes[scene];
}
