/* port/3ds/game/gdx3ds_nrr_dress_game.c -- [native] nrr_dress: the game side of the course dressing
 * on the NRR (gfx/nrr/gdx3ds_nrr_fx.h, docs/research/native/n60fx-progress.md).
 *
 * Race_Draw's DRESS section (Course_GadgetsDraw) is consumed (gdx3ds_c5tick_consume). Its only
 * persistent write is sCourseDecorationTextureLoadState (reset at the decoration pass, c5tick §3.1).
 * The track is not consumed through c5tick (nrr_trk keeps Course_Draw's cull and lean walk), so
 * c5tick's "trk needs dress" hold is not involved, and this stub reads the game's own cull --
 * every chunk's drawState, written by Course_Draw just before -- exactly as the stock pass does.
 *
 * In place of the gadget pass (mines, jumps, course effects: ~7 commands per strip pair) it writes:
 *   [learn]  for a gadget block it needs that is not learned: the stock setup lists in order with a
 *            learn point per kind, under identity matrices (model-view pushed and popped, the
 *            projection reloaded);
 *   marker   one core marker over the snapshot (per vertex load: the course buffer, kind, pattern,
 *            the strip pairs the cull keeps and the ones that get the wrap ST patch);
 *   tail     the state the stock gadget pass leaves: its setup lists (inside the gadget direct-load
 *            scope), the jump texture load, the last course-effect tile.
 * The decoration pass (gates, signs, buildings) follows as the stock commands, written by this stub
 * with the same expressions and calls as Course_GadgetsDraw (its texture-load state included).
 *
 * Scope: the 1P race outside the course editor; everything else gets Course_GadgetsDraw.
 * Compiled into gdx3ds_game (decomp include paths and types). */
#include "global.h"
#include "fzx_game.h"
#include "fzx_racer.h"
#include "fzx_course.h"
#include ASSET_HEADER(course_track_gfx.h)
#include ASSET_HEADER(setup_gfx.h)

#include "nrr/gdx3ds_nrr.h"
#include "nrr/gdx3ds_nrr_deco.h"
#include "nrr/gdx3ds_nrr_fx.h"

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
typedef void* (*GdxDlbStubFn)(void* gfx, int sec, int view);
extern int gdx3ds_c5tick_consume(int sec, GdxDlbStubFn stub);
#ifdef EXPANSION_KIT
extern int gdx3ds_c5tick_testdrive_scope(void); /* S5b: gdx3ds_c5tick.h */
#endif
extern int gdx_dress_scope_marker(void* slot, int scope, int open);
/* [native] nrr_deco (gfx/nrr/gdx3ds_nrr_mach.cpp): 1 when the category's render half is on. */
extern int gdx3ds_nrr_deco_on(void) __attribute__((weak));

/* course_gadgets.c's state (external linkage there). */
extern s32 sCourseDecorationTextureLoadState;
extern Gfx* (*sCourseDecorationDrawFuncs[])(Gfx*);
extern CourseDecoration gCourseDecorations[];
extern SegmentChunk gSegmentChunks[];
extern CourseEffectsInfo gCourseEffectsInfo;
extern CourseFeaturesInfo gCourseFeaturesInfo;
/* The Expansion Kit doubles gEffectsDrawData ([2][192], the editor's per-frame copy; the race reads
 * row 0, the address used here) and narrows the chunk map D_800E12C8 to s16 (course_gadgets.c):
 * read as s32 here, every strip pair tested the wrong chunk (DD-1's sand patch drew nothing). */
extern EffectDrawData gEffectsDrawData[];
#ifdef EXPANSION_KIT
extern s16 D_800E12C8[];
#else
extern s32 D_800E12C8[];
#endif
extern unk_80225800 D_80225800;
extern bool gInCourseEditor;
extern bool Course_FeatureIsDecorational(s32 courseFeature);

extern GfxPool D_1000000;
extern GfxPool D_8024DCE0[];
extern GfxPool* gGfxPool;
extern s32 gNumPlayers;

#define NRRDRESS_READY gdx3ds_nrr_fx_ready[2]

/* The Expansion Kit's gadget pass (course_gadgets.c, its EXPANSION_KIT branches) differs from the
 * ROM game's in three state commands, and the stub mirrors each (o3dsek-plan.md section 5):
 *   - after the landmine setup list: G_TF_BILERP and a model-view load of D_2000000 (the ROM game
 *     inherits both from the track);
 *   - each course-effect tile at texture scale 0xFFFF (the ROM game: 0x8000).
 * The filter persists into the decoration pass, so the stub writes it whatever the tail mode. */
#ifdef EXPANSION_KIT
extern Mtx D_2000000[];
#define NRRDRESS_CE_SCALE 0xFFFF
#else
#define NRRDRESS_CE_SCALE 0x8000
#endif
#define NRRDRESS_SCOPE_GADGET 1 /* gdx3ds_dress.h GDXDRESS_SCOPE_GADGET */
#define NRRDRESS_SCOPE_DECO 2   /* gdx3ds_dress.h GDXDRESS_SCOPE_DECO */

static Mtx sIdentityMtx;
static Vtx sLearnVtx[3] = {
    { { { 0, 0, 0 }, 0, { 0, 0 }, { 255, 255, 255, 255 } } },
    { { { 1, 0, 0 }, 0, { 32, 0 }, { 255, 255, 255, 255 } } },
    { { { 0, 1, 0 }, 0, { 0, 32 }, { 255, 255, 255, 255 } } },
};
static GdxNrrDressItem sItems[2][GDX_NRRDRESS_MAX_ITEMS];
/* niview: views 1-3 of a multi-view race, per pool, allocated at the first multi-view build. */
static GdxNrrDressItem* sItemsView[2][NRR_MAX_VIEWS - 1];

static GdxNrrDressItem* NrrDressItemsFor(s32 pool, s32 view) {
    GdxNrrDressItem** slot;
    if (view <= 0) {
        return sItems[pool];
    }
    slot = &sItemsView[pool][view - 1];
    if (*slot == NULL) {
        *slot = (GdxNrrDressItem*) gdx3ds_nrr_view_alloc(GDX_NRRDRESS_MAX_ITEMS * sizeof(GdxNrrDressItem));
    }
    return *slot;
}
static int sTail = 0; /* [native] nrr_dress_tail (default 0, lean): 1 = the gadget pass's setup lists + jump load + last
                         tile after the marker (exact by construction); 0 = lean: none */
static u32 sLearns;
static u32 sStockFrames;
static u32 sOverflow;

/* ---- the gadget pass: what the stock commands would draw, recorded -------------------------- */

typedef struct NrrDressBuild {
    GdxNrrDressItem* out;
    u32 n;
    u32 cap;
    u32 verts; /* packed vertices the items can emit at most */
    u8 want[GDX_NRRDRESS_K_COUNT]; /* kinds this frame draws */
    s32 lastTile;                  /* the last course-effect gSPTexture tile, -1 none */
} NrrDressBuild;

static GdxNrrDressItem* NrrDressAdd(NrrDressBuild* b, const Vtx* v, u32 kind, u32 pat, u32 nv) {
    GdxNrrDressItem* e;
    if (b->n >= b->cap) {
        sOverflow++;
        return NULL;
    }
    e = &b->out[b->n++];
    memset(e, 0, sizeof(*e));
    e->vtx = v;
    e->kind = (u8) kind;
    e->pat = (u8) pat;
    e->nv = (u8) nv;
    b->want[kind] = 1;
    b->verts += pat == GDX_NRRDRESS_PAT_STRIP ? 90u : (pat == GDX_NRRDRESS_PAT_JUMP ? 18u : 12u);
    return e;
}

static void NrrDressMines(NrrDressBuild* b) {
    Vtx* vtx;
    if (gCourseFeaturesInfo.landmineCount == 0) {
        return;
    }
    vtx = &D_80225800.landmineVtx[(gCourseFeaturesInfo.landmineCount - 1) * 5];
    do {
        NrrDressAdd(b, vtx, GDX_NRRDRESS_K_MINE, GDX_NRRDRESS_PAT_MINE, 5);
        vtx -= 5;
    } while (vtx >= D_80225800.landmineVtx);
}

static void NrrDressJumps(NrrDressBuild* b) {
    Vtx* vtx;
    if (gCourseFeaturesInfo.jumpCount == 0) {
        return;
    }
    vtx = &D_80225800.jumpVtx[(gCourseFeaturesInfo.jumpCount - 1) * 6];
    do {
        NrrDressAdd(b, vtx, GDX_NRRDRESS_K_JUMP, GDX_NRRDRESS_PAT_JUMP, 6);
        vtx -= 6;
    } while (vtx >= D_80225800.jumpVtx);
}

/* The wrap test of one strip pair (the stock tc[1] comparison). */
static int NrrDressPairWraps(const Vtx* v) {
    return (v[2].v.tc[1] < v[0].v.tc[1]) || (v[3].v.tc[1] < v[1].v.tc[1]);
}

/* One 32-vertex (or remainder) group of a course-effect strip: the pairs the chunk cull keeps
 * (gSegmentChunks[D_800E12C8[k + first]].drawState) and the wrap patches. */
static void NrrDressStripGroup(NrrDressBuild* b, u32 kind, Vtx* vtx, s32 nv, s32 pairs, s32 first, s32 st2) {
    GdxNrrDressItem* e = NrrDressAdd(b, vtx, kind, GDX_NRRDRESS_PAT_STRIP, (u32) nv);
    s32 k;
    if (e == NULL) {
        return;
    }
    e->st2 = (u8) st2;
    for (k = 0; k < pairs; k++) {
        if (gSegmentChunks[D_800E12C8[k + first]].drawState == 0) {
            continue;
        }
        e->pairs |= (u16) (1u << k);
        if (NrrDressPairWraps(&vtx[k << 1])) {
            e->mods |= (u16) (1u << k);
        }
    }
}

/* One course effect: the stock grouping into 32-vertex loads and the remainder. */
static void NrrDressStrip(NrrDressBuild* b, EffectDrawData* d, u32 kind) {
    s32 numVtxs = d->vtxEnd - d->vtxStart;
    s32 groups = (numVtxs >> 5) + 1;
    s32 rem = numVtxs & 0x1F;
    Vtx* vtx = d->vtxStart;
    s32 j;
    if (rem == 0) {
        rem = 32;
        groups--;
    }
    for (j = 0; j < groups - 1; j++) {
        s32 first = (d->vtxStart - D_80225800.terrainEffectVtx + (j << 5)) >> 1;
        NrrDressStripGroup(b, kind, vtx, 32, 15, first, 0);
        vtx += 32;
    }
    {
        s32 first = (d->vtxStart - D_80225800.terrainEffectVtx + (j << 5)) >> 1;
        NrrDressStripGroup(b, kind, d->vtxStart + (j << 5), rem, (rem >> 1) - 1, first, 1);
    }
}

static void NrrDressCourseEffects(NrrDressBuild* b) {
    EffectDrawData* d;
    EffectDrawData* end;
    Vtx* dashVtx = D_80225800.dashVtx;
    if (gCourseEffectsInfo.count == 0) {
        return;
    }
    end = gEffectsDrawData + gCourseEffectsInfo.count;
    for (d = gEffectsDrawData; d < end; d++) {
        u32 kind = GDX_NRRDRESS_K_CE0 + (d->effectType & 7);
        b->lastTile = d->effectType;
        if (d->effectType == COURSE_EFFECT_DASH) {
            NrrDressAdd(b, dashVtx, kind, GDX_NRRDRESS_PAT_DASH, 6);
            dashVtx += 6;
            continue;
        }
        NrrDressStrip(b, d, kind);
    }
}

/* ---- learn ---------------------------------------------------------------------------------- */

static Gfx* NrrDressLearnMarker(Gfx* gfx, u32 op, u32 kind) {
    gfx->words.w0 = ((u32) G_NOOP << 24) | (GDX_NRRFX_LEARN_P << 16) | (op << 8) | (u32) NRR_CAT_DRESS;
    gfx->words.w1 = (GfxW1) kind;
    return gfx + 1;
}

static Gfx* NrrDressLearnPoint(Gfx* gfx, u32 kind) {
    gfx = NrrDressLearnMarker(gfx, GDX_NRRFX_OP_LEARN_BEGIN, kind);
    gSP2Triangles(gfx++, 0, 1, 2, 0, 0, 2, 1, 0);
    return NrrDressLearnMarker(gfx, GDX_NRRFX_OP_LEARN_END, kind);
}

static int NrrDressNeedLearn(const NrrDressBuild* b) {
    u32 k;
    for (k = 0; k < GDX_NRRDRESS_K_COUNT; k++) {
        if (b->want[k] && !NRRDRESS_READY[k]) {
            return 1;
        }
    }
    return 0;
}

/* The gadget pass's setup chain in stock order, a learn point per wanted kind not learned yet. */
static Gfx* NrrDressWriteLearn(Gfx* gfx, const NrrDressBuild* b, s32 view) {
    u32 t;
    sLearns++;
    gSPMatrix(gfx++, &sIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(gfx++, &sIdentityMtx, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPVertex(gfx++, sLearnVtx, 3, 0);
    gSPDisplayList(gfx++, aSetupLandmineTextureDL);
#ifdef EXPANSION_KIT
    gDPSetTextureFilter(gfx++, G_TF_BILERP); /* not the D_2000000 load: the learn point needs identity */
#endif
    if (b->want[GDX_NRRDRESS_K_MINE] && !NRRDRESS_READY[GDX_NRRDRESS_K_MINE]) {
        gfx = NrrDressLearnPoint(gfx, GDX_NRRDRESS_K_MINE);
    }
    if (gCourseFeaturesInfo.jumpCount != 0) {
        gDPLoadTextureBlock(gfx++, aJumpFeatureTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        if (b->want[GDX_NRRDRESS_K_JUMP] && !NRRDRESS_READY[GDX_NRRDRESS_K_JUMP]) {
            gfx = NrrDressLearnPoint(gfx, GDX_NRRDRESS_K_JUMP);
        }
    }
    if (gCourseEffectsInfo.count != 0) {
        gSPDisplayList(gfx++, aSetupCourseEffectTextureDL);
        for (t = 0; t < 8; t++) {
            u32 kind = GDX_NRRDRESS_K_CE0 + t;
            if (b->want[kind] && !NRRDRESS_READY[kind]) {
                gSPTexture(gfx++, NRRDRESS_CE_SCALE, NRRDRESS_CE_SCALE, 0, t, G_ON);
                gfx = NrrDressLearnPoint(gfx, kind);
            }
        }
    }
    gSPPopMatrix(gfx++, G_MTX_MODELVIEW);
    gSPMatrix(gfx++, &D_1000000.unk_20208[view], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    return gfx;
}

#ifdef EXPANSION_KIT
/* The Expansion Kit pass's own state before its gadgets (the model view the marker's vertices are
 * transformed under, the filter the decorations inherit). The ROM game writes neither. */
static Gfx* NrrDressEkState(Gfx* gfx) {
    gDPSetTextureFilter(gfx++, G_TF_BILERP);
    gSPMatrix(gfx++, D_2000000, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    return gfx;
}
#endif

/* ---- tail: the gadget pass's state, then the stock decoration pass -------------------------- */

static Gfx* NrrDressScope(Gfx* gfx, int scope, int open) {
    if (gdx_dress_scope_marker(gfx, scope, open)) {
        gfx++;
    }
    return gfx;
}

static Gfx* NrrDressGadgetTail(Gfx* gfx, const NrrDressBuild* b) {
    gfx = NrrDressScope(gfx, NRRDRESS_SCOPE_GADGET, 1);
    gSPDisplayList(gfx++, aSetupLandmineTextureDL);
#ifdef EXPANSION_KIT
    gDPSetTextureFilter(gfx++, G_TF_BILERP);
#endif
    if (gCourseFeaturesInfo.jumpCount != 0) {
        gDPLoadTextureBlock(gfx++, aJumpFeatureTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    }
    if (gCourseEffectsInfo.count != 0) {
        gSPDisplayList(gfx++, aSetupCourseEffectTextureDL);
        if (b->lastTile >= 0) {
            gSPTexture(gfx++, NRRDRESS_CE_SCALE, NRRDRESS_CE_SCALE, 0, b->lastTile, G_ON);
        }
    }
    return NrrDressScope(gfx, NRRDRESS_SCOPE_GADGET, 0);
}

static Gfx* NrrDressDayNightMode(Gfx* gfx) {
    gDPSetCombineMode(gfx++, G_CC_BLENDRGBA, G_CC_BLENDRGBA);
    gDPSetRenderMode(gfx++, Z_CMP | Z_UPD | CVG_DST_FULL | ZMODE_OPA | ALPHA_CVG_SEL | FORCE_BL | G_RM_FOG_SHADE_A,
                     Z_CMP | Z_UPD | CVG_DST_FULL | ZMODE_OPA | ALPHA_CVG_SEL | FORCE_BL |
                         GBL_c2(G_BL_CLR_FOG, G_BL_A_SHADE, G_BL_CLR_IN, G_BL_1MA));
    return gfx;
}

/* One decoration loop of Course_GadgetsDraw (gates/signs, or buildings), stock. */
static Gfx* NrrDressDecorations(Gfx* gfx, int buildings) {
    CourseFeaturesInfo* info = &gCourseFeaturesInfo;
    CourseFeature* feature;
    CourseFeature* featuresEnd = info->features + info->featureCount;
    CourseDecoration* decoration = gCourseDecorations;
    Mtx* decorationMtx = D_80225800.decorationMtx;
    for (feature = info->features; feature < featuresEnd; feature++) {
        int pass;
        if (!Course_FeatureIsDecorational(feature->featureType)) {
            continue;
        }
        pass = buildings ? COURSE_FEATURE_IS_BUILDING(feature->featureType)
                         : (feature->featureType <= COURSE_FEATURE_SIGN_OVERHEAD);
        if ((decoration->loadChunk->drawState != 0) && pass) {
            gSPMatrix(gfx++, decorationMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gfx = sCourseDecorationDrawFuncs[feature->featureType](gfx);
        }
        decorationMtx++;
        decoration++;
    }
    return gfx;
}

static Gfx* NrrDressDecorationPass(Gfx* gfx) {
    s32 night = COURSE_CONTEXT()->courseData.skybox == SKYBOX_NIGHT;
    gfx = NrrDressScope(gfx, NRRDRESS_SCOPE_DECO, 1);
    gSPSetGeometryMode(gfx++, G_CULL_BACK);
    gSPTexture(gfx++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gSPFogPosition(gfx++, 980, 1000);
    gDPPipeSync(gfx++);
    if (night) {
        gDPSetCombineMode(gfx++, G_CC_DECALRGBA, G_CC_DECALRGBA);
        gDPSetRenderMode(gfx++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
    } else {
        gfx = NrrDressDayNightMode(gfx);
    }
    sCourseDecorationTextureLoadState = -1;
    gfx = NrrDressDecorations(gfx, 0);
    if (night) {
        gDPPipeSync(gfx++);
        gfx = NrrDressDayNightMode(gfx);
    }
    gfx = NrrDressDecorations(gfx, 1);
    return NrrDressScope(gfx, NRRDRESS_SCOPE_DECO, 0);
}

/* ---- [native] nrr_deco: the decoration passes on the NRR (gfx/nrr/gdx3ds_nrr_deco.h) ---------
 *
 * The plan walks both decoration loops exactly as Course_GadgetsDraw does and simulates the stock
 * stream's texture state: the load memo (sCourseDecorationTextureLoadState) and the TMEM owner
 * (the last loader: a memo setup list / building load, or a self-loading body). Each visible
 * decoration gets its record key (type, owner at its draw). A decoration whose key has a complete
 * record is drawn by the category at the marker; the rest are written as the stock commands after
 * it, in stock order, each with the owner the stock stream gives it (a forced memo miss reloads
 * its setup list; NinTex left gets its loader re-emitted), bracketed by REC markers when its key
 * is not recorded yet. A stock decoration whose owner the reduced stream cannot rebuild (a memo
 * hit after a self-loading body) turns the whole frame's passes stock. The memo ends at the stock
 * stream's value (its only persistent write). */

#define NRRDECO_OWNER_NONE (-1)
#define NRRDECO_OWNER_GATE_START 6
#define NRRDECO_OWNER_SIGN_TV 8

typedef struct NrrDecoPlanEnt {
    const Mtx* base;
    s8 type;
    s8 owner;  /* the TMEM owner the stock stream draws it with */
    u8 native; /* drawn by the category (its key is recorded) */
    u8 building;
} NrrDecoPlanEnt;

typedef struct NrrDecoPlan {
    NrrDecoPlanEnt e[GDX_NRRDECO_MAX];
    s32 n;
    s32 memo;  /* the stock stream's sCourseDecorationTextureLoadState */
    s32 owner; /* the stock stream's TMEM owner */
    u32 sig;
    s32 nNative;
    s32 cold;  /* recordable, not recorded yet */
    s32 never; /* self-loading bodies (stock every frame) */
} NrrDecoPlan;

static int sDeco = -1; /* [native] nrr_deco, latched at the first build (its category inits after dress) */
static u32 sDecoSig = 0;
static u32 sDecoFullStock = 0;

/* The memo code a type's draw function tests and sets (its setup list or building load), -1 none. */
static s32 NrrDecoMemoCode(s32 type) {
    static const s8 kMemo[GDX_NRRDECO_TYPES] = { 5,  -1, 7, -1, -1, 9, 9, 10, 10, 11, -1,
                                                 12, 0,  0, 1,  1,  2, 2, 3,  3,  4,  4 };
    return kMemo[type];
}

/* The owner code of a body that loads its own textures (never recorded: loads between its
 * triangles), -1 for the others. */
static s32 NrrDecoSelfLoader(s32 type) {
    if (type == COURSE_FEATURE_GATE_START) {
        return NRRDECO_OWNER_GATE_START;
    }
    if (type == COURSE_FEATURE_SIGN_TV_RIGHT || type == COURSE_FEATURE_SIGN_TV_LEFT) {
        return NRRDECO_OWNER_SIGN_TV;
    }
    return -1;
}

/* Recorded bodies a type draws: the NinTex and overhead signs a second one under a rotation. */
static u32 NrrDecoDraws(s32 type) {
    return (type >= COURSE_FEATURE_SIGN_NINTEX_RIGHT && type <= COURSE_FEATURE_SIGN_OVERHEAD) ? 2u : 1u;
}

/* The rotation the second body multiplies onto the decoration's matrix (the GfxPool's, as the
 * draw function's G_MTX_MUL of D_1000000.unk_2C5E8 / 2C628 / 2C668 resolves). */
static const Mtx* NrrDecoMul(s32 type) {
    if (type == COURSE_FEATURE_SIGN_NINTEX_RIGHT) {
        return gGfxPool->unk_2C5E8;
    }
    if (type == COURSE_FEATURE_SIGN_NINTEX_LEFT) {
        return gGfxPool->unk_2C628;
    }
    if (type == COURSE_FEATURE_SIGN_OVERHEAD) {
        return gGfxPool->unk_2C668;
    }
    return NULL;
}

static u32 NrrDecoFnv(u32 h, u32 v) {
    return (h ^ v) * 16777619u;
}

/* One decoration of the stock stream: the memo and owner before and after its draw. */
static void NrrDecoPlanAdd(NrrDecoPlan* p, const Mtx* base, s32 type, int building) {
    NrrDecoPlanEnt* e;
    s32 code = NrrDecoMemoCode(type);
    s32 self = NrrDecoSelfLoader(type);
    if (p->n >= GDX_NRRDECO_MAX) {
        return;
    }
    if (code >= 0 && p->memo != code) {
        p->owner = code; /* the setup list / load block runs */
    }
    if (code >= 0) {
        p->memo = code;
    }
    if (self >= 0) {
        p->owner = self;
    }
    e = &p->e[p->n++];
    e->base = base;
    e->type = (s8) type;
    e->owner = (s8) p->owner;
    e->native = 0;
    e->building = (u8) building;
}

/* One decoration loop of Course_GadgetsDraw (gates/signs, or buildings), planned. */
static void NrrDecoPlanLoop(NrrDecoPlan* p, int buildings) {
    CourseFeaturesInfo* info = &gCourseFeaturesInfo;
    CourseFeature* feature;
    CourseFeature* featuresEnd = info->features + info->featureCount;
    CourseDecoration* decoration = gCourseDecorations;
    Mtx* decorationMtx = D_80225800.decorationMtx;
    for (feature = info->features; feature < featuresEnd; feature++) {
        int pass;
        if (!Course_FeatureIsDecorational(feature->featureType)) {
            continue;
        }
        if (buildings) {
            const s32* w = (const s32*) decorationMtx;
            p->sig = NrrDecoFnv(NrrDecoFnv(NrrDecoFnv(p->sig, (u32) feature->featureType), (u32) w[6]), (u32) w[7]);
        }
        pass = buildings ? COURSE_FEATURE_IS_BUILDING(feature->featureType)
                         : (feature->featureType <= COURSE_FEATURE_SIGN_OVERHEAD);
        if ((decoration->loadChunk->drawState != 0) && pass) {
            NrrDecoPlanAdd(p, decorationMtx, feature->featureType, buildings);
        }
        decorationMtx++;
        decoration++;
    }
}

/* Which decorations the category draws: a recorded key; the course signature first (a course
 * change drops every record). */
static void NrrDecoChoose(NrrDecoPlan* p) {
    s32 i;
    if (p->sig != sDecoSig) {
        sDecoSig = p->sig;
        gdx3ds_nrr_deco_course(p->sig);
    }
    for (i = 0; i < p->n; i++) {
        NrrDecoPlanEnt* e = &p->e[i];
        if (NrrDecoSelfLoader(e->type) >= 0) {
            p->never++;
        } else if (gdx3ds_nrr_deco_ready(GDX_NRRDECO_KEY(e->type, e->owner))) {
            e->native = 1;
            p->nNative++;
        } else {
            p->cold++;
        }
    }
}

/* 1 when every stock decoration of the plan can be written with the owner the stock stream gives
 * it, once the native ones are left out. */
static int NrrDecoStockFeasible(const NrrDecoPlan* p) {
    s32 eo = NRRDECO_OWNER_NONE;
    s32 i;
    for (i = 0; i < p->n; i++) {
        const NrrDecoPlanEnt* e = &p->e[i];
        s32 code = NrrDecoMemoCode(e->type);
        s32 self = NrrDecoSelfLoader(e->type);
        if (e->native) {
            continue;
        }
        if (self >= 0) {
            eo = self;
        } else if (code >= 0) {
            if (e->owner != code) {
                return 0; /* a memo hit over a self-loading body's textures */
            }
            eo = code;
        } else if (e->owner != eo) {
            if (e->owner == NRRDECO_OWNER_NONE || e->owner == NRRDECO_OWNER_GATE_START ||
                e->owner == NRRDECO_OWNER_SIGN_TV) {
                return 0; /* NinTex left over textures no loader of ours rebuilds */
            }
            eo = e->owner;
        }
    }
    return 1;
}

/* A building's texture load (Course_FeatureDrawBuilding*), owner 0..4. */
static Gfx* NrrDecoBuildingLoad(Gfx* gfx, s32 owner) {
    gDPPipeSync(gfx++);
    if (owner == 0) {
        gDPLoadTextureBlock(gfx++, D_8014A20, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    } else if (owner == 1) {
        gDPLoadTextureBlock(gfx++, D_801F018, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    } else if (owner == 2) {
        gDPLoadTextureBlock(gfx++, D_801F8C8, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    } else if (owner == 3) {
        gDPLoadTextureBlock(gfx++, D_8020208, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 64, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
    } else {
        gDPLoadTextureBlock(gfx++, D_8021380, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 64, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
    }
    return gfx;
}

/* The loader the stock stream ran for owner `owner` (a memo code 0..12, not a self-loading body),
 * written alone: its setup list, or the building's load block. */
static Gfx* NrrDecoLoader(Gfx* gfx, s32 owner) {
    if (owner >= 0 && owner <= 4) {
        return NrrDecoBuildingLoad(gfx, owner);
    }
    if (owner == 5) {
        gSPDisplayList(gfx++, D_8017220);
    } else if (owner == 7) {
        gSPDisplayList(gfx++, D_801A1F8);
    } else if (owner == 9) {
        gSPDisplayList(gfx++, D_801D030);
    } else if (owner == 10) {
        gSPDisplayList(gfx++, D_801E178);
    } else if (owner == 11) {
        gSPDisplayList(gfx++, D_3000688);
    } else if (owner == 12) {
        gSPDisplayList(gfx++, D_3000590);
    }
    return gfx;
}

static Gfx* NrrDecoRecMarker(Gfx* gfx, u32 op, u32 w1) {
    gfx->words.w0 = ((u32) G_NOOP << 24) | (GDX_NRRDECO_REC_P << 16) | (op << 8);
    gfx->words.w1 = (GfxW1) w1;
    return gfx + 1;
}

/* The stock streams' state while the stock decorations are written: TMEM owner and memo. */
typedef struct NrrDecoEmit {
    s32 owner;
    s32 memo;
} NrrDecoEmit;

/* One decoration as the stock commands, with the stock stream's owner: a memo type reloads its
 * setup unless the reduced stream's last loader was its own; NinTex left gets its loader. */
static Gfx* NrrDecoStockOne(Gfx* gfx, const NrrDecoPlanEnt* e, NrrDecoEmit* em) {
    s32 code = NrrDecoMemoCode(e->type);
    s32 self = NrrDecoSelfLoader(e->type);
    s32 key = GDX_NRRDECO_KEY(e->type, e->owner);
    int rec = self < 0;
    if (self < 0 && code < 0 && e->owner != em->owner) {
        gfx = NrrDecoLoader(gfx, e->owner);
        em->owner = e->owner;
    }
    sCourseDecorationTextureLoadState = code >= 0 ? (em->owner == code ? code : -1) : em->memo;
    if (rec) {
        gfx = NrrDecoRecMarker(gfx, GDX_NRRDECO_OP_BEGIN, (u32) key | (NrrDecoDraws(e->type) << 16));
    }
    gSPMatrix(gfx++, e->base, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gfx = sCourseDecorationDrawFuncs[e->type](gfx);
    if (rec) {
        gfx = NrrDecoRecMarker(gfx, GDX_NRRDECO_OP_END, (u32) key);
    }
    em->memo = sCourseDecorationTextureLoadState;
    if (self >= 0) {
        em->owner = self;
    } else if (code >= 0) {
        em->owner = code;
    }
    return gfx;
}

/* The stock decorations of the plan: the pass prologue, then both loops' stock members in stock
 * order (the night combiner reset between them, as stock). */
static Gfx* NrrDecoStockPasses(Gfx* gfx, const NrrDecoPlan* p) {
    s32 night = COURSE_CONTEXT()->courseData.skybox == SKYBOX_NIGHT;
    NrrDecoEmit em;
    int resetDone = 0;
    s32 i;
    em.owner = NRRDECO_OWNER_NONE;
    em.memo = -1;
    gfx = NrrDressScope(gfx, NRRDRESS_SCOPE_DECO, 1);
    gSPSetGeometryMode(gfx++, G_CULL_BACK);
    gSPTexture(gfx++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gSPFogPosition(gfx++, 980, 1000);
    gDPPipeSync(gfx++);
    if (night) {
        gDPSetCombineMode(gfx++, G_CC_DECALRGBA, G_CC_DECALRGBA);
        gDPSetRenderMode(gfx++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
    } else {
        gfx = NrrDressDayNightMode(gfx);
    }
    for (i = 0; i < p->n; i++) {
        const NrrDecoPlanEnt* e = &p->e[i];
        if (night && e->building && !resetDone) {
            gDPPipeSync(gfx++);
            gfx = NrrDressDayNightMode(gfx);
            resetDone = 1;
        }
        if (!e->native) {
            gfx = NrrDecoStockOne(gfx, e, &em);
        }
    }
    if (night && !resetDone) {
        gDPPipeSync(gfx++);
        gfx = NrrDressDayNightMode(gfx);
    }
    return NrrDressScope(gfx, NRRDRESS_SCOPE_DECO, 0);
}

/* The snapshot of the native decorations (stock order). */
static void NrrDecoFillSnap(GdxNrrDecoSnap* s, const NrrDecoPlan* p) {
    s32 i;
    memset(s, 0, sizeof(*s));
    s->sig = p->sig;
    for (i = 0; i < p->n; i++) {
        const NrrDecoPlanEnt* e = &p->e[i];
        GdxNrrDecoEnt* d;
        if (!e->native) {
            continue;
        }
        d = &s->e[s->n++];
        d->base = (const int32_t*) e->base;
        d->mul = (const int32_t*) NrrDecoMul(e->type);
        d->key = (u16) GDX_NRRDECO_KEY(e->type, e->owner);
    }
}

/* [native] nrr_deco: both decoration passes, native where recorded. */
static Gfx* NrrDecoPass(Gfx* gfx, s32 view) {
    NrrDecoPlan p;
    GdxNrrDecoSnap snap;
    s32 i;
    int full = 0;
    memset(&p, 0, sizeof(p));
    p.memo = -1;
    p.owner = NRRDECO_OWNER_NONE;
    p.sig = 2166136261u ^ (u32) COURSE_CONTEXT()->courseData.skybox;
    NrrDecoPlanLoop(&p, 0);
    NrrDecoPlanLoop(&p, 1);
    NrrDecoChoose(&p);
    if (p.nNative != 0 && !NrrDecoStockFeasible(&p)) {
        for (i = 0; i < p.n; i++) {
            p.e[i].native = 0;
        }
        p.nNative = 0;
        full = 1;
        sDecoFullStock++;
    }
    NrrDecoFillSnap(&snap, &p);
    gfx = (Gfx*) gdx3ds_nrr_deco_publish(gfx, &snap, p.cold, p.never, full, view);
    if (p.nNative != p.n) {
        gfx = NrrDecoStockPasses(gfx, &p);
    }
    sCourseDecorationTextureLoadState = p.memo;
    return gfx;
}

/* ---- the stub ------------------------------------------------------------------------------- */

static int NrrDressNativeScope(s32 view) {
    if (!gdx3ds_nrr_view_ok(NRR_CAT_DRESS, view, gNumPlayers) || gdx3ds_nrr_fx_broken[2]) { /* niview: 2-4P */
        return 0;
    }
#ifdef EXPANSION_KIT
    if (gInCourseEditor) {
        return gdx3ds_c5tick_testdrive_scope(); /* S5b: the editor's test drive race, nothing else */
    }
#endif
    return !gInCourseEditor;
}

static void* NrrDressStub(void* gfxv, int sec, int view) {
    Gfx* gfx = (Gfx*) gfxv;
    NrrFrame* fr;
    GdxNrrDressSnap* snap;
    NrrDressBuild b;
    s32 pool = (s32) (gGfxPool - D_8024DCE0);
    (void) sec;
    fr = (NrrDressNativeScope(view) && pool >= 0 && pool < 2) ? gdx3ds_nrr_frame_build() : NULL;
    snap = fr != NULL ? (GdxNrrDressSnap*) NrrSnapAlloc(fr, sizeof(GdxNrrDressSnap)) : NULL;
    if (snap == NULL) {
        sStockFrames++;
        return NULL; /* out of scope, or the frame arena is full: Course_GadgetsDraw builds */
    }
    memset(&b, 0, sizeof(b));
    b.out = NrrDressItemsFor(pool, view);
    if (b.out == NULL) {
        sStockFrames++;
        return NULL; /* no memory for this view's items: Course_GadgetsDraw builds */
    }
    snap->multi = gNumPlayers > 1 ? 1 : 0;
    b.cap = GDX_NRRDRESS_MAX_ITEMS;
    b.lastTile = -1;
    NrrDressMines(&b);
    NrrDressJumps(&b);
    NrrDressCourseEffects(&b);
    snap->items = b.out;
    snap->n = (u16) b.n;
    snap->capVerts = b.verts;
    if (NrrDressNeedLearn(&b)) {
        gfx = NrrDressWriteLearn(gfx, &b, view);
    }
#ifdef EXPANSION_KIT
    gfx = NrrDressEkState(gfx);
#endif
    if (b.n != 0) {
        NrrSnapSet(fr, NRR_CAT_DRESS, view, snap);
        gfx = (Gfx*) gdx3ds_nrr_write_marker(gfx, NRR_CAT_DRESS, view, fr);
    }
    if (sTail) {
        gfx = NrrDressGadgetTail(gfx, &b);
    }
    if (sDeco < 0) {
        sDeco = (&gdx3ds_nrr_deco_on != NULL && gdx3ds_nrr_deco_on()) ? 1 : 0;
    }
    if (sDeco && (gNumPlayers == 1 || gdx3ds_nrr_ni_view(NRR_CAT_DECO))) {
        return NrrDecoPass(gfx, view); /* niview: a 2-4P view's decorations need ni_view's deco bit */
    }
    return NrrDressDecorationPass(gfx);
}

/* ---- the category init (core contract: called once at boot when nrr_dress is on) ----------- */

/* n60stereo (gdx3ds_stereo_frame.cpp): 1 = the NRR may draw under the [stereo] config. */
int gdx3ds_stereo_nrr_ok(void) __attribute__((weak));

void gdx3ds_nrr_cat_init_dress(void) {
    int taken;
    guMtxIdent(&sIdentityMtx);
    if (&gdx3ds_stereo_nrr_ok != NULL && !gdx3ds_stereo_nrr_ok()) {
        /* [stereo] enabled with [native] stereo_replay=0: the core draws mono only
         * (gdx3ds_nrr_bk_frame_active), a consumed section would vanish from a stereo frame, so the
         * section stays stock (as nrr_mach). With the frame replay (n60stereo) it draws both eyes. */
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[nrr] %s init REFUSED: [stereo] enabled, stereo_replay=0 (mono only); the section stays "
                         "stock\n", "dress");
        }
        return;
    }
    sTail = &gdx3ds_config_get_int != NULL ? gdx3ds_config_get_int("native", "nrr_dress_tail", 0) : 0;
    gdx3ds_nrr_dress_render_init();
    taken = gdx3ds_c5tick_consume(GDX_DLB_SEC_DRESS, NrrDressStub);
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[nrr] dress init consume=%d tail=%d (1P race: gadget pass native -- mines, jumps, course "
                     "effects with the game's chunk cull; decorations stock commands unless [native] nrr_deco; "
                     "4P / editor stock)\n",
                     taken, sTail);
    }
}

/* [nrrfx] receipt counters (game side): learn sequences written, stock-built sections, items
 * that did not fit the snapshot. */
void gdx3ds_nrr_dress_game_counts(unsigned long out[3]) {
    out[0] = sLearns;
    out[1] = sStockFrames;
    out[2] = sOverflow;
}
