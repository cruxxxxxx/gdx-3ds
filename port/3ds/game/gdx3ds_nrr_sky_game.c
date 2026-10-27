/* port/3ds/game/gdx3ds_nrr_sky_game.c -- [native] nrr_sky: the game side of the NRR sky
 * (gfx/nrr/gdx3ds_nrr_sky.h, docs/research/native/n60sky-progress.md).
 *
 * Race_Draw's SKY section is consumed (gdx3ds_c5tick_consume, c5tick-progress.md §3.1/§3.5:
 * Background_Draw writes no game state; Background_Update -- logic -- already computed every
 * vertex, scroll and sprite matrix into the GfxPool). In its place this stub writes, for the
 * 1P race only:
 *
 *   [learn frame]  the stock sky's STATE commands in stock order with a dummy triangle (identity
 *                  matrices, three host vertices) between LEARN markers at each draw point;
 *   head           Background_Draw's head, unchanged: perspective normalize, projection,
 *                  Camera_Draw, the sky setup list, the model-view load (the track reads them);
 *   DRAW marker(s) w1 = this GfxPool's snapshot; part A (gradient, day sprites), then whatever
 *                  stays legacy (night sprites, stars) as stock commands, then part B (floor,
 *                  clouds) -- one marker for both when nothing sits between;
 *   tail           the stock sky's final render mode / combine / prim / TLUT / persp state
 *                  (no texture loads, no triangles).
 *
 * Every other caller and scope (2P-4P, the ending, records, the course editor, the lever off,
 * a broken learn) gets Background_Draw itself. Snapshot = straight-line copies on the game fiber.
 * Compiled into gdx3ds_game (decomp include paths and types). */
#include "global.h"
#include "fzx_game.h"
#include "fzx_camera.h"
#include "overlays/ovl_i3/background.h"
#include ASSET_HEADER(machine_custom_gfx.h)

#include "nrr/gdx3ds_nrr.h"
#include "nrr/gdx3ds_nrr_sky.h"

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
typedef void* (*GdxDlbStubFn)(void* gfx, int sec, int view);
extern int gdx3ds_c5tick_consume(int sec, GdxDlbStubFn stub);

/* background.c's state (external linkage there). */
extern BackgroundContext sBackgroundCtx;
extern s16 sIsEndingCutscene;
extern s16 sBackgroundSpriteCount;
extern s16 sSpritePaletteReplacementCount;
extern s16 sBackgroundSpriteR;
extern s16 sBackgroundSpriteG;
extern s16 sBackgroundSpriteB;
extern u16 sSkyboxFlags;
extern s32 sCloudCount;
extern TexturePtr sSkyboxTexture;
extern TexturePtr sVenueFloorTexture;
extern TexturePtr sCloudTexture;
extern TexturePtr sStarTexture;
extern BackgroundSprite sBackgroundSprites[6];
extern Star sStars[100];
extern s32 sStarCount;

extern GfxPool* gGfxPool;
extern GfxPool D_8024DCE0[];
extern GfxPool D_1000000;
extern Mtx D_2000000[];
extern Camera gCameras[];
extern s32 gNumPlayers;
extern void gdx_fv_map_point(int view, f32* x, f32* y); /* gdx3ds_fullview_game.c */
extern s32 gGameMode;
extern s32 gCourseIndex;
extern u32 gGameFrameCount;

static u32 sContentHash;         /* what the last learn was made from */
static u32 sLearnSerial;         /* the identity the snapshot carries; 0 = never learned */
static u32 sLearns;
static int sLean = -1; /* [native] nrr_sky_lean (default 1): no setup DL / MV load in the head, no tail; 0 = both (n60sky-progress §4) */
static u32 sStockFrames;
static Mtx sIdentityMtx;
static Vtx sLearnVtx[3] = {
    { { { 0, 0, 0 }, 0, { 0, 0 }, { 255, 255, 255, 255 } } },
    { { { 1, 0, 0 }, 0, { 32, 0 }, { 255, 255, 255, 255 } } },
    { { { 0, 1, 0 }, 0, { 0, 32 }, { 255, 255, 255, 255 } } },
};

/* ---- markers ------------------------------------------------------------------------------ */

/* A learn marker (p=0x64, this category's cold path; gfx/nrr/gdx3ds_nrr_sky.h). */
static Gfx* NrrSkyLearnMarker(Gfx* gfx, u32 op, u32 w1) {
    gfx->words.w0 = ((u32) G_NOOP << 24) | (GDX_NRRSKY_LEARN_P << 16) | (op << 8);
    gfx->words.w1 = (GfxW1) w1;
    return gfx + 1;
}

/* The dummy triangle (both windings: whatever the cull mode, one survives) between the markers. */
static Gfx* NrrSkyLearnPoint(Gfx* gfx, u32 kind) {
    gfx = NrrSkyLearnMarker(gfx, GDX_NRRSKY_OP_LEARN_BEGIN, kind);
    gSP2Triangles(gfx++, 0, 1, 2, 0, 0, 2, 1, 0);
    return NrrSkyLearnMarker(gfx, GDX_NRRSKY_OP_LEARN_END, (sLearnSerial << 8) | kind);
}

/* ---- the learn sequence: Background_Draw's state commands, in its order -------------------- */

static int NrrSkySpritesNative(void) {
    return sBackgroundSpriteCount != 0 && (sSkyboxFlags & SKYBOX_NIGHTTIME) == 0;
}

/* What the stock stars and night sprites leave in the RDP before the floor (their draws stay
 * stock between the parts; the floor's block must be learned under the same preceding state). */
static Gfx* NrrSkyLearnLegacyState(Gfx* gfx) {
    if (sSkyboxFlags & SKYBOX_STARRY) {
        /* Background_DrawStars' state; two learn rectangles fit its screen -> clip map */
        gSPDisplayList(gfx++, D_303AA18);
        gDPLoadTextureBlock(gfx++, sStarTexture, G_IM_FMT_IA, G_IM_SIZ_8b, 8, 8, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                            G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        gDPPipeSync(gfx++);
        gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255, 255);
        gfx = NrrSkyLearnMarker(gfx, GDX_NRRSKY_OP_LEARN_BEGIN, GDX_NRRSKY_K_STAR);
        gSPTextureRectangle(gfx++, GDX_NRRSKY_STAR_LEARN_XL, GDX_NRRSKY_STAR_LEARN_YL,
                            GDX_NRRSKY_STAR_LEARN_XL + 32, GDX_NRRSKY_STAR_LEARN_YL + 32, 0, 0, 0, 1 << 10, 1 << 10);
        gSPTextureRectangle(gfx++, GDX_NRRSKY_STAR_LEARN_XL + GDX_NRRSKY_STAR_LEARN_DX,
                            GDX_NRRSKY_STAR_LEARN_YL + GDX_NRRSKY_STAR_LEARN_DY,
                            GDX_NRRSKY_STAR_LEARN_XL + GDX_NRRSKY_STAR_LEARN_DX + 32,
                            GDX_NRRSKY_STAR_LEARN_YL + GDX_NRRSKY_STAR_LEARN_DY + 32, 0, 0, 0, 1 << 10, 1 << 10);
        gfx = NrrSkyLearnMarker(gfx, GDX_NRRSKY_OP_LEARN_END, (sLearnSerial << 8) | GDX_NRRSKY_K_STAR);
        gDPPipeSync(gfx++);
        gDPSetTexturePersp(gfx++, G_TP_PERSP);
    }
    if (sBackgroundSpriteCount != 0 && !NrrSkySpritesNative()) {
        gDPPipeSync(gfx++);
        gDPSetRenderMode(gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
        gDPSetCombineLERP(gfx++, 0, 0, 0, TEXEL0, TEXEL0, 0, SHADE, 0, 0, 0, 0, TEXEL0, TEXEL0, 0, SHADE, 0);
        gDPSetPrimColor(gfx++, 0, 0, sBackgroundSpriteR, sBackgroundSpriteG, sBackgroundSpriteB, 255);
        gDPPipeSync(gfx++);
        gDPSetTextureLUT(gfx++, G_TT_NONE);
    }
    return gfx;
}

static Gfx* NrrSkyLearnSprites(Gfx* gfx) {
    s32 i;
    gDPPipeSync(gfx++);
    gDPSetRenderMode(gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineLERP(gfx++, TEXEL0, 0, PRIMITIVE, 0, TEXEL0, 0, SHADE, 0, TEXEL0, 0, PRIMITIVE, 0, TEXEL0, 0, SHADE,
                      0);
    gDPSetPrimColor(gfx++, 0, 0, sBackgroundSpriteR, sBackgroundSpriteG, sBackgroundSpriteB, 255);
    gDPSetTextureLUT(gfx++, G_TT_RGBA16);
    for (i = 0; i < sBackgroundSpriteCount; i++) {
        gDPPipeSync(gfx++);
        gDPLoadTLUT_pal16(gfx++, 0, sBackgroundSprites[i].palette);
        gDPLoadTextureBlock_4b(gfx++, sBackgroundSprites[i].texture, G_IM_FMT_CI, 64, 64, 0,
                               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK,
                               G_TX_NOLOD, G_TX_NOLOD);
        gfx = NrrSkyLearnPoint(gfx, GDX_NRRSKY_K_SPRITE0 + i);
    }
    gDPPipeSync(gfx++);
    gDPSetTextureLUT(gfx++, G_TT_NONE);
    return gfx;
}

static Gfx* NrrSkyLearnFloorClouds(Gfx* gfx) {
    CourseSkyboxes* skybox = sBackgroundCtx.skybox;
    gSPDisplayList(gfx++, D_303AA40);
    gDPSetPrimColor(gfx++, 0, 0, skybox->courseFogR, skybox->courseFogG, skybox->courseFogB, 0);
    gDPLoadTextureBlock(gfx++, sVenueFloorTexture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 32, 0, G_TX_NOMIRROR | G_TX_WRAP,
                        G_TX_NOMIRROR | G_TX_WRAP, 6, 5, G_TX_NOLOD, G_TX_NOLOD);
    gfx = NrrSkyLearnPoint(gfx, GDX_NRRSKY_K_FLOOR);
    if (sCloudCount > 0) {
        gDPPipeSync(gfx++);
        gDPSetRenderMode(gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
        gDPSetCombineLERP(gfx++, PRIMITIVE, 0, TEXEL0, 0, 0, TEXEL0, SHADE, TEXEL0, PRIMITIVE, 0, TEXEL0, 0, 0, TEXEL0,
                          SHADE, TEXEL0);
        gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255, 0);
        gDPLoadTextureBlock(gfx++, sCloudTexture, G_IM_FMT_IA, G_IM_SIZ_8b, 64, 32, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, 6, 5, G_TX_NOLOD, G_TX_NOLOD);
        gfx = NrrSkyLearnPoint(gfx, GDX_NRRSKY_K_CLOUD);
    }
    return gfx;
}


static Gfx* NrrSkyWriteLearn(Gfx* gfx) {
    sLearnSerial = (sLearnSerial + 1) & 0xFFFFFFu;
    if (sLearnSerial == 0) {
        sLearnSerial = 1;
    }
    sLearns++;
    gSPMatrix(gfx++, &sIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(gfx++, &sIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(gfx++, D_303A9E0);
    gSPVertex(gfx++, sLearnVtx, 3, 0);
    gDPLoadTextureBlock(gfx++, sSkyboxTexture, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 1, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                        G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gfx = NrrSkyLearnPoint(gfx, GDX_NRRSKY_K_GRAD);
    gfx = NrrSkyLearnLegacyState(gfx);
    if (NrrSkySpritesNative()) {
        gfx = NrrSkyLearnSprites(gfx);
    }
    return NrrSkyLearnFloorClouds(gfx);
}

/* ---- head, legacy middle, tail ------------------------------------------------------------ */

static s32 NrrSkyScissorFor(s32 view);

static Gfx* NrrSkyWriteHead(Gfx* gfx, s32 view) {
    gSPPerspNormalize(gfx++, gCameras[view].perspectiveScale);
    gSPMatrix(gfx++, &D_1000000.unk_20208[view], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gfx = Camera_Draw(gfx, NrrSkyScissorFor(view), view); /* 1P: SCISSOR_BOX_FULL_SCREEN */
    if (sLean == 1 && gNumPlayers > 1) {
        /* niview: views 1-3 follow the previous view's track and gadgets, whose model-view is still
         * loaded; the stock head's identity MV makes the walker's MP this view's projection again. */
        gSPMatrix(gfx++, D_2000000, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        return gfx;
    }
    if (sLean == 1) {
#ifdef EXPANSION_KIT
        /* The lean head leaves the model view to Race_Draw's setup list. The ROM game's
         * (D_303A8F8, an asset) loads D_2000000; the Expansion Kit's (D_8076CF10, compiled C)
         * carries the raw segment-2 address 0x02000000 instead. On the port that address does not
         * reach D_2000000's live storage (the bridge aliases only the D_2000000 symbol to
         * D_80225800; segment 2 is gUnkBssVramStart). The stock sky reloads it anyway, so only the
         * NRR head saw the wrong model view: the sky and the track drew wrong through every race
         * intro (o3dsek-plan.md section 5). Load it here, as the stock sky does. */
        gSPMatrix(gfx++, D_2000000, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
#endif
        return gfx;
    }
    gSPDisplayList(gfx++, D_303A9E0);
    gSPMatrix(gfx++, D_2000000, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    return gfx;
}

/* The night sprites stay stock commands (their white palette index flashes every frame, a new
 * texture-cache key each frame): Background_DrawBackgroundSprites' night branch, minus the sprites
 * whose billboard is wholly behind the eye (the stock clipper emits nothing for those). */
static int NrrSkySpriteBehind(const BackgroundSprite* s);

static Gfx* NrrSkyWriteNightSprites(Gfx* gfx) {
    BackgroundSprite* sp;
    s32 i;
    gDPPipeSync(gfx++);
    gDPSetRenderMode(gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineLERP(gfx++, 0, 0, 0, TEXEL0, TEXEL0, 0, SHADE, 0, 0, 0, 0, TEXEL0, TEXEL0, 0, SHADE, 0);
    gDPSetPrimColor(gfx++, 0, 0, sBackgroundSpriteR, sBackgroundSpriteG, sBackgroundSpriteB, 255);
    gDPSetTextureLUT(gfx++, G_TT_RGBA16);
    for (i = 0, sp = sBackgroundSprites; i < sBackgroundSpriteCount; i++, sp++) {
        if (!sp->isVisible || NrrSkySpriteBehind(sp)) {
            continue;
        }
        gSPMatrix(gfx++, &D_1000000.unk_2C368[i], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPVertex(gfx++, D_1000000.unk_2C4E8, 4, 0);
        gDPPipeSync(gfx++);
        gDPLoadTLUT_pal16(gfx++, 0, D_1000000.unk_2C528[sp->replacementIndex]);
        gDPLoadTextureBlock_4b(gfx++, sp->texture, G_IM_FMT_CI, 64, 64, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        gSP2Triangles(gfx++, 0, 3, 1, 0, 0, 2, 3, 0);
    }
    gDPPipeSync(gfx++);
    gDPSetTextureLUT(gfx++, G_TT_NONE);
    gSPMatrix(gfx++, D_2000000, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    return gfx;
}

static int NrrSkyHasLegacyMiddle(void) {
    return sBackgroundSpriteCount != 0 && !NrrSkySpritesNative();
}

/* The RDP state the stock sky leaves for the track (render mode, combine, prim, TLUT mode). */
static Gfx* NrrSkyWriteTail(Gfx* gfx) {
    CourseSkyboxes* skybox = sBackgroundCtx.skybox;
    if (sLean == 1) {
        return gfx;
    }
    if (NrrSkySpritesNative()) {
        gDPSetTextureLUT(gfx++, G_TT_NONE);
    }
    gSPDisplayList(gfx++, D_303AA40);
    gDPSetPrimColor(gfx++, 0, 0, skybox->courseFogR, skybox->courseFogG, skybox->courseFogB, 0);
    if (sCloudCount > 0) {
        gDPSetRenderMode(gfx++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
        gDPSetCombineLERP(gfx++, PRIMITIVE, 0, TEXEL0, 0, 0, TEXEL0, SHADE, TEXEL0, PRIMITIVE, 0, TEXEL0, 0, 0, TEXEL0,
                          SHADE, TEXEL0);
        gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255, 0);
    }
    return gfx;
}

/* ---- snapshot ------------------------------------------------------------------------------ */

static u32 NrrSkyMix(u32 h, u32 v) {
    return (h ^ v) * 16777619u;
}

/* Everything a learned block depends on; a change re-learns. */
static u32 NrrSkyContentHash(void) {
    u32 h = 2166136261u;
    s32 i;
    h = NrrSkyMix(h, (u32) (uintptr_t) sSkyboxTexture);
    h = NrrSkyMix(h, (u32) (uintptr_t) sVenueFloorTexture);
    h = NrrSkyMix(h, (u32) (uintptr_t) sCloudTexture);
    h = NrrSkyMix(h, (u32) (uintptr_t) sBackgroundCtx.skybox);
    h = NrrSkyMix(h, ((u32) sSkyboxFlags << 16) | ((u32) sCloudCount << 8) | (u32) sBackgroundSpriteCount);
    h = NrrSkyMix(h, ((u32) gCourseIndex << 8) | (u32) gGameMode);
    for (i = 0; i < sBackgroundSpriteCount; i++) {
        h = NrrSkyMix(h, (u32) (uintptr_t) sBackgroundSprites[i].texture);
        h = NrrSkyMix(h, (u32) (uintptr_t) sBackgroundSprites[i].palette);
    }
    return h;
}

/* ---- the render-side math that moved to the snapshot (n60sky-progress §6.2) ---------------- */

/* == n64/gdx_rsp.cpp SpMatrix's fixed-point load (host-endian GfxPool Mtx). */
static void NrrSkyMtxToFloat(const Mtx* m, f32 out[4][4]) {
    const s32* w = (const s32*) m;
    s32 i;
    for (i = 0; i < 8; i++) {
        s32 ip = w[i];
        u32 fp = (u32) w[8 + i];
        out[i / 2][(i & 1) * 2] = (s32) ((ip & 0xffff0000) | (fp >> 16)) / 65536.0f;
        out[i / 2][(i & 1) * 2 + 1] = (s32) ((ip << 16) | (fp & 0xffff)) / 65536.0f;
    }
}

/* == gdx_rsp.cpp MatrixMul (res = a x b). */
static void NrrSkyMatMul(f32 res[4][4], f32 a[4][4], f32 b[4][4]) {
    s32 r;
    s32 k;
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 4; k++) {
            res[r][k] = a[r][0] * b[0][k] + a[r][1] * b[1][k] + a[r][2] * b[2][k] + a[r][3] * b[3][k];
        }
    }
}

/* The stock clipper's polygon reduced to position and colour (gdx_tri.cpp ClipDistance /
 * Interpolate arithmetic, colour bytes truncated after each step). */
typedef struct NrrSkyClipV {
    f32 p[4];
    f32 c[4];
} NrrSkyClipV;

static f32 NrrSkyClipDist(const NrrSkyClipV* v, s32 plane) {
    switch (plane) {
        case 0:
            return v->p[3] - 1.0e-5f;
        case 1:
            return v->p[0] + v->p[3];
        case 2:
            return v->p[3] - v->p[0];
        case 3:
            return v->p[1] + v->p[3];
        case 4:
            return v->p[3] - v->p[1];
        case 5:
            return v->p[2] + v->p[3];
        default:
            return v->p[3] - v->p[2];
    }
}

static void NrrSkyClipLerp(const NrrSkyClipV* a, const NrrSkyClipV* b, f32 t, NrrSkyClipV* r) {
    s32 k;
    for (k = 0; k < 4; k++) {
        f32 v = a->c[k] + (b->c[k] - a->c[k]) * t;
        r->p[k] = a->p[k] + (b->p[k] - a->p[k]) * t;
        r->c[k] = (f32) (u8) (v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    }
}

/* Sutherland-Hodgman over the 7 stock planes; poly[0] ends as the vertex SpTri's fan emits first. */
static s32 NrrSkyClipPoly(NrrSkyClipV* poly, NrrSkyClipV* tmp, s32 count) {
    NrrSkyClipV* in = poly;
    NrrSkyClipV* out = tmp;
    NrrSkyClipV* sw;
    s32 plane;
    for (plane = 0; plane < 7 && count >= 3; plane++) {
        s32 i;
        s32 n = 0;
        f32 cd = NrrSkyClipDist(&in[0], plane);
        for (i = 0; i < count; i++) {
            s32 ni = (i + 1 == count) ? 0 : i + 1;
            f32 nd = NrrSkyClipDist(&in[ni], plane);
            s32 ci = cd >= 0.0f;
            if (ci) {
                out[n++] = in[i];
            }
            if (ci != (nd >= 0.0f)) {
                f32 den = cd - nd;
                f32 t = den != 0.0f ? cd / den : 0.0f;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                NrrSkyClipLerp(&in[i], &in[ni], t, &out[n++]);
            }
            cd = nd;
        }
        count = n;
        sw = in;
        in = out;
        out = sw;
    }
    if (in != poly && count > 0) {
        poly[0] = in[0];
    }
    return count;
}

/* The floor batch's vertex-0 colour: the floor combiner reads shade as a vertex-0 constant in the
 * packed layout, and the stock batch's vertex 0 is the first vertex the CPU clipper emits for the
 * first surviving floor triangle. Clip space = the head's MP (identity MV x the projection Mtx,
 * converted as SpMatrix does), x times TransformVertices' aspect factor. */
static void NrrSkySnapFloorShade(GdxNrrSkyFrame* f, f32 p[4][4]) {
    static const u8 kTris[4][3] = { { 0, 3, 1 }, { 0, 2, 3 }, { 4, 7, 5 }, { 4, 6, 7 } };
    NrrSkyClipV poly[16];
    NrrSkyClipV tmp[16];
    const Vtx* v = (const Vtx*) f->vtx; /* the view's 28 background vertices */
    f32 xs = gdx3ds_nrr_sky_xs;
    s32 t;
    s32 j;
    s32 k;
    s32 count = 0;
    for (t = 0; t < 4 && count < 3; t++) {
        for (j = 0; j < 3; j++) {
            const Vtx_t* s = &v[kTris[t][j]].v;
            for (k = 0; k < 4; k++) {
                poly[j].p[k] = s->ob[0] * p[0][k] + s->ob[1] * p[1][k] + s->ob[2] * p[2][k] + p[3][k];
                poly[j].c[k] = s->cn[k];
            }
            poly[j].p[0] *= xs;
        }
        count = NrrSkyClipPoly(poly, tmp, 3);
    }
    f->floorShadeOk = count >= 3;
    for (k = 0; k < 4; k++) {
        f->floorShade[k] = (u8) poly[0].c[k];
    }
}

/* A sprite whose whole billboard (half-diagonal 32 * 50 * sqrt 2 < 2300 world units) lies behind
 * the eye: every corner has w < 0 and the stock clipper emits nothing for it. Its matrix and
 * quad are not worth sending (5 of 6 sprites in a GP frame, c5sky-progress.md §1). */
static int NrrSkySpriteBehind(const BackgroundSprite* s) {
    const Camera* cam = &gCameras[0];
    f32 front = s->pos.x * cam->basis.x.x + (400.0f - cam->eye.y) * cam->basis.x.y + s->pos.z * cam->basis.x.z;
    return front < -2300.0f;
}

/* The visible stars of the view (Background_UpdateStars, logic): position and colour. The 1P race's
 * view is 0; a multi-view race's star screen positions already carry the view's offset (niview). */
static void NrrSkySnapStars(NrrFrame* fr, GdxNrrSkyFrame* f, s32 view) {
    GdxNrrSkyStar* out = (GdxNrrSkyStar*) NrrSnapAlloc(fr, sizeof(GdxNrrSkyStar) * (u32) sStarCount);
    Star* st;
    s32 i;
    u32 n = 0;
    if (out == NULL) {
        return;
    }
    for (i = 0, st = sStars; i < sStarCount; i++, st++) {
        if (st->isVisible[view]) {
            /* MP phase 3: the local view of a fullview build maps the split-screen position to the
             * full-screen viewport (Background_DrawStars does the same; identity otherwise). */
            f32 left = st->screenPosition[view].left;
            f32 top = st->screenPosition[view].top;
            gdx_fv_map_point(view, &left, &top);
            out[n].xl = (s16) (s32) (left * 4.0f);
            out[n].yl = (s16) (s32) (top * 4.0f);
            out[n].rgba[0] = st->red;
            out[n].rgba[1] = st->green;
            out[n].rgba[2] = st->blue;
            out[n].rgba[3] = st->alpha;
            n++;
        }
    }
    f->stars = out;
    f->starCount = (u8) n;
}

static GdxNrrSkyFrame* NrrSkySnap(NrrFrame* fr, s32 view) {
    GdxNrrSkyFrame* f = (GdxNrrSkyFrame*) NrrSnapAlloc(fr, sizeof(GdxNrrSkyFrame));
    f32 p[4][4];
    f32 mv[4][4];
    s32 i;
    if (f == NULL) {
        return NULL;
    }
    f->frame = gGameFrameCount;
    f->identity = sLearnSerial;
    f->vtx = &gGfxPool->unk_29B48[view * 28];
    f->spriteVtx = &gGfxPool->unk_2C4E8[0];
    f->flags = (sCloudCount > 0) ? GDX_NRRSKY_F_CLOUDS : 0;
    f->flags |= NrrSkyHasLegacyMiddle() ? GDX_NRRSKY_F_SPLIT : 0;
    f->flags |= gNumPlayers > 1 ? GDX_NRRSKY_F_VIEW : 0; /* niview: no split (the scope refuses it) */
    NrrSkyMtxToFloat(&gGfxPool->unk_20208[view], p);
    NrrSkySnapFloorShade(f, p);
    f->spriteCount = 0;
    f->starCount = 0;
    f->stars = NULL;
    if (NrrSkySpritesNative() && view == 0) { /* Background_Draw: the sprites in view 0 only */
        for (i = 0; i < sBackgroundSpriteCount; i++) {
            if (sBackgroundSprites[i].isVisible && !NrrSkySpriteBehind(&sBackgroundSprites[i])) {
                NrrSkyMtxToFloat(&gGfxPool->unk_2C368[i], mv);
                NrrSkyMatMul(f->sprites[f->spriteCount].mp, mv, p);
                f->sprites[f->spriteCount].kind = (u8) (GDX_NRRSKY_K_SPRITE0 + i);
                f->spriteCount++;
            }
        }
    }
    if (sSkyboxFlags & SKYBOX_STARRY) {
        NrrSkySnapStars(fr, f, view);
    }
    NrrSnapSet(fr, NRR_CAT_SKY, view, f);
    if (f->flags & GDX_NRRSKY_F_SPLIT) {
        NrrSnapSet(fr, NRR_CAT_SKY, 1, f);
    }
    return f;
}

/* ---- the stub ------------------------------------------------------------------------------ */

int gdx3ds_c5tick_scene_scope(void); /* niview: the records flyby's scope (gdx3ds_c5tick.c) */

static s32 NrrSkyScissorFor(s32 view) {
    static const s8 kTwo[2] = { SCISSOR_BOX_TOP_HALF, SCISSOR_BOX_BOTTOM_HALF };
    static const s8 kFour[4] = { SCISSOR_BOX_TOP_LEFT_QUARTER, SCISSOR_BOX_BOTTOM_LEFT_QUARTER,
                                 SCISSOR_BOX_TOP_RIGHT_QUARTER, SCISSOR_BOX_BOTTOM_RIGHT_QUARTER };
    if (gNumPlayers == 2) {
        return kTwo[view & 1];
    }
    if (gNumPlayers >= 3) {
        return kFour[view & 3];
    }
    return SCISSOR_BOX_FULL_SCREEN;
}

/* niview ([native] ni_view bit NRR_CAT_SKY): a view of a 2-4P race (VS, the attract demos), without
 * the night sprites (stock commands between two parts: one split per view is not modelled). */
static int NrrSkyMultiViewScope(s32 view) {
    if (gNumPlayers < 2 || gNumPlayers > NRR_MAX_VIEWS || view < 0 || view >= gNumPlayers) {
        return 0;
    }
    if (!gdx3ds_nrr_ni_view(NRR_CAT_SKY)) {
        return 0;
    }
    return !NrrSkyHasLegacyMiddle();
}

/* The native scope: the 1P race's one view (or a multi-view race's, niview), no ending cutscene, no
 * records flyby, not broken. */
static int NrrSkyNativeScope(s32 view) {
    if (gNumPlayers != 1 && !NrrSkyMultiViewScope(view)) {
        return 0;
    }
    if ((gNumPlayers == 1 && view != 0) || sIsEndingCutscene) {
        return 0;
    }
    if (gGameMode == GAMEMODE_GP_END_CS || (gGameMode == GAMEMODE_RECORDS && !gdx3ds_c5tick_scene_scope())) {
        return 0;
    }
    return !gdx3ds_nrr_sky_broken;
}

static void* NrrSkyStub(void* gfxv, int sec, int view) {
    Gfx* gfx = (Gfx*) gfxv;
    NrrFrame* fr;
    u32 content;
    (void) sec;
    fr = NrrSkyNativeScope(view) ? gdx3ds_nrr_frame_build() : NULL;
    if (fr == NULL) {
        sStockFrames++;
        return Background_Draw(gfx, view, NrrSkyScissorFor(view));
    }
    content = NrrSkyContentHash();
    if (sLearnSerial == 0 || content != sContentHash || gdx3ds_nrr_sky_relearn) {
        sContentHash = content;
        gdx3ds_nrr_sky_relearn = 0;
        gfx = NrrSkyWriteLearn(gfx);
    }
    if (NrrSkySnap(fr, view) == NULL) {
        sStockFrames++; /* the frame arena is full: this frame's sky is the stock one */
        return Background_Draw(gfx, view, NrrSkyScissorFor(view));
    }
    gfx = NrrSkyWriteHead(gfx, view);
    gfx = gdx3ds_nrr_write_marker(gfx, NRR_CAT_SKY, view, fr);
    if (NrrSkyHasLegacyMiddle()) {
        gfx = NrrSkyWriteNightSprites(gfx);
        gfx = gdx3ds_nrr_write_marker(gfx, NRR_CAT_SKY, 1, fr);
    }
    return NrrSkyWriteTail(gfx);
}

/* ---- the category init (core contract: called once at boot when nrr_sky is on) ------------- */

void gdx3ds_nrr_cat_init_sky(void) {
    int taken;
    guMtxIdent(&sIdentityMtx);
    sLean = &gdx3ds_config_get_int != NULL ? gdx3ds_config_get_int("native", "nrr_sky_lean", 1) : 1;
    gdx3ds_nrr_sky_render_init();
    taken = gdx3ds_c5tick_consume(GDX_DLB_SEC_SKY, NrrSkyStub);
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[nrr] sky init consume=%d (1P race: SKY consumed, learned state blocks, PICA clip; "
                     "night sprites stock commands between parts)\n",
                     taken);
    }
}

/* [nrr] sky receipt counters (game side): learn sequences written, stock-built skies. */
void gdx3ds_nrr_sky_game_counts(unsigned long out[2]) {
    out[0] = sLearns;
    out[1] = sStockFrames;
}
