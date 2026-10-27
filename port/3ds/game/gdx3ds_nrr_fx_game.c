/* port/3ds/game/gdx3ds_nrr_fx_game.c -- [native] nrr_fx: the game side of the boost flames and the
 * race effects on the NRR (gfx/nrr/gdx3ds_nrr_fx.h, docs/research/native/n60fx-progress.md).
 *
 * BOOST (nrr_fx_parts bit 1). Racer_Draw's booster section is consumed (gdx3ds_c5tick_consume). Its
 * only writes are the GfxPool's effects vertex buffer and its cursor gEffectsVtxPtr (c5tick §3.2);
 * this stub writes both exactly as the stock loops do (same expressions, same order: the effects
 * section after it reads the cursor, and later builds on this pool half read the bytes this one
 * leaves), records per racer where its vertices are, its triangle pattern and env colour, and in
 * place of the stock commands writes:
 *   [learn]  when the boost block is not learned: aSetupBoosterDL + a learn point, under identity
 *            matrices (model-view pushed and popped, the projection reloaded);
 *   setup    aSetupBoosterDL, as stock: it loads the matrices the flames and the effects are
 *            transformed with (the marker reads the walker's MP) and the state the effects
 *            bracket inherits (combiner, texture);
 *   marker   the fx category's core marker for this view (the effects' snapshot rides the same
 *            root: the render side reads it after the whole build);
 *   tail     the last env colour the stock section sets.
 *
 * Scope: the 1P race outside the GP ending and the course editor (the stub returns NULL = stock).
 * Compiled into gdx3ds_game (decomp include paths and types). */
#include "global.h"
#include "fzx_racer.h"
#include "fzx_game.h"
#include "fzx_camera.h"
#include "fzx_math.h"
#include "fzx_effects.h"
#include ASSET_HEADER(hud_gfx.h)


#include "nrr/gdx3ds_nrr.h"
#include "nrr/gdx3ds_nrr_fx.h"

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
typedef void* (*GdxDlbStubFn)(void* gfx, int sec, int view);
extern int gdx3ds_c5tick_consume(int sec, GdxDlbStubFn stub);
#ifdef EXPANSION_KIT
extern int gdx3ds_c5tick_testdrive_scope(void); /* S5b: gdx3ds_c5tick.h */
#endif

extern GfxPool D_1000000;
extern GfxPool D_8024DCE0[];
extern GfxPool* gGfxPool;
extern Vtx* gEffectsVtxEndPtr;
extern TexturePtr D_800CDAB8[];
extern Racer* sLastRacer;
extern GhostRacer gGhostRacers[3];
extern BoosterInfo sBoosterInfos[];
extern s32 sInitialBoostTimer;
extern f32 D_800F80A4;
extern Vtx* gEffectsVtxPtr;
extern Camera gCameras[];
extern s32 gGamePaused;
extern u32 gGameFrameCount;
extern s32 gGameMode;
extern s32 gNumPlayers;

#define NRRFX_READY gdx3ds_nrr_fx_ready[1]

static int sParts = 0; /* [native] nrr_fx_parts */
static int sTail = 0;  /* [native] nrr_fx_tail (default 0, lean): 1 = every class's setup list + last registers after the
                          marker (the stock bracket's final state, exact by construction); 0 = lean:
                          nothing (the fall explosions' own setup list follows anyway) */
static Mtx sIdentityMtx;
static Vtx sLearnVtx[3] = {
    { { { 0, 0, 0 }, 0, { 0, 0 }, { 255, 255, 255, 255 } } },
    { { { 1, 0, 0 }, 0, { 32, 0 }, { 255, 255, 255, 255 } } },
    { { { 0, 1, 0 }, 0, { 0, 32 }, { 255, 255, 255, 255 } } },
};
static u32 sLearns;
static u32 sStockFrames;
static u32 sOverflow;

/* ---- the per-view root ------------------------------------------------------------------- */

static GdxNrrFxSnap* NrrFxSnapFor(NrrFrame* fr, s32 view) {
    GdxNrrFxSnap* s = (GdxNrrFxSnap*) NrrSnapGet(fr, NRR_CAT_FX, view);
    if (s != NULL) {
        return s;
    }
    s = (GdxNrrFxSnap*) NrrSnapAlloc(fr, sizeof(GdxNrrFxSnap));
    if (s != NULL) {
        s->multi = gNumPlayers > 1 ? 1 : 0; /* niview: a view of a multi-view race */
        NrrSnapSet(fr, NRR_CAT_FX, view, s);
    }
    return s;
}

/* ---- the boost flames: the stock vertex writes, recorded ---------------------------------- */

/* The camera axes the stock loops scale the flame quads along (Racer_Draw sets them right after
 * the body section: up = basis.y, side = basis.z). */
typedef struct NrrBoostCam {
    f32 ux, uy, uz;
    f32 sx, sy, sz;
} NrrBoostCam;

typedef struct NrrBoostBuild {
    GdxNrrFxSnap* snap;
    NrrBoostCam cam;
    s32 haveEnv;
    u8 env[4];
} NrrBoostBuild;

/* One booster's quad (+ the trail vertex), exactly the stock expressions; returns the cursor. */
static Vtx* NrrBoostQuad(Vtx* v, Racer* racer, BoosterInfo* info, s32 i, f32 flare, f32 scale,
                         const NrrBoostCam* c) {
    Vec3f* pos = &info->pos[i];
    f32 size = info->size[i] * 0.35f;
    f32 x = ((racer->modelMatrix.m[0][0] * pos->x) + (racer->modelMatrix.m[1][0] * pos->y) +
             (racer->modelMatrix.m[2][0] * (pos->z - (size * 3.6f)))) +
            racer->modelMatrix.m[3][0];
    f32 y = ((racer->modelMatrix.m[0][1] * pos->x) + (racer->modelMatrix.m[1][1] * pos->y) +
             (racer->modelMatrix.m[2][1] * (pos->z - (size * 3.6f)))) +
            racer->modelMatrix.m[3][1];
    f32 z = ((racer->modelMatrix.m[0][2] * pos->x) + (racer->modelMatrix.m[1][2] * pos->y) +
             (racer->modelMatrix.m[2][2] * (pos->z - (size * 3.6f)))) +
            racer->modelMatrix.m[3][2];
    f32 d0;
    f32 d1;
    f32 d2;

    size += flare;
    size *= scale;
    d0 = size * c->ux;
    v[0].v.ob[0] = Math_Round(x + d0);
    d1 = size * c->uy;
    v[0].v.ob[1] = Math_Round(y + d1);
    d2 = size * c->uz;
    v[0].v.ob[2] = Math_Round(z + d2);
    v[2].v.ob[0] = Math_Round(x - d0);
    v[2].v.ob[1] = Math_Round(y - d1);
    v[2].v.ob[2] = Math_Round(z - d2);
    d0 = size * c->sx;
    v[1].v.ob[0] = Math_Round(x + d0);
    d1 = size * c->sy;
    v[1].v.ob[1] = Math_Round(y + d1);
    d2 = size * c->sz;
    v[1].v.ob[2] = Math_Round(z + d2);
    v[3].v.ob[0] = Math_Round(x - d0);
    v[3].v.ob[1] = Math_Round(y - d1);
    v[3].v.ob[2] = Math_Round(z - d2);
    v[0].v.tc[0] = v[1].v.tc[0] = v[1].v.tc[1] = v[2].v.tc[1] = 0;
    v[0].v.tc[1] = v[2].v.tc[0] = v[3].v.tc[0] = v[3].v.tc[1] = 0x3FF;
    if (racer->unk_2B3 == 2) {
        f32 trail = racer->speed * 0.9f;
        v[4].v.ob[0] = Math_Round(x - (racer->modelBasis.x.x * trail));
        v[4].v.ob[1] = Math_Round(y - (racer->modelBasis.x.y * trail));
        v[4].v.ob[2] = Math_Round(z - (racer->modelBasis.x.z * trail));
        v[4].v.tc[0] = 0x3FF;
        v[4].v.tc[1] = 0;
        return v + 5;
    }
    return v + 4;
}

static void NrrBoostRecord(NrrBoostBuild* b, Racer* racer, BoosterInfo* info, const Vtx* start) {
    GdxNrrBoostEnt* e;
    if (b->snap->nBoost >= b->snap->capBoost) {
        sOverflow++;
        return;
    }
    e = &b->snap->boost[b->snap->nBoost++];
    e->vtx = start;
    e->count = (u8) info->count;
    e->trail = racer->unk_2B3 == 2 ? 1 : 0;
    memcpy(e->env, b->env, 4);
}

static void NrrBoostSetEnv(NrrBoostBuild* b, s32 r, s32 g, s32 bl, s32 a) {
    b->env[0] = (u8) r;
    b->env[1] = (u8) g;
    b->env[2] = (u8) bl;
    b->env[3] = (u8) a;
    b->haveEnv = 1;
}

/* The racers' flames: the stock loop (Racer_Draw, booster section) minus the commands. */
static void NrrBoostRacers(NrrBoostBuild* b, s32 frame) {
    Racer* racer;
    for (racer = sLastRacer; racer >= gRacers; racer--) {
        BoosterInfo* info;
        Vtx* start = gEffectsVtxPtr;
        s32 flicker;
        f32 flare;
        f32 boost;
        f32 scale;
        s32 i;
        if ((racer->unk_2B3 == 0) || (racer->stateFlags & (RACER_STATE_CRASHED | RACER_STATE_SPINNING_OUT))) {
            continue;
        }
        info = &sBoosterInfos[racer->boostersType];
        flicker = (3 - (frame & 3)) << 1;
        if (flicker < racer->unk_214) {
            flicker = racer->unk_214;
        }
        flare = flicker * 0.2f;
        boost = (f32) racer->boostTimer / sInitialBoostTimer;
        if (boost != 0) {
            flare += (7.5f * sqrtf(boost));
            if (racer->stateFlags & RACER_STATE_DASH_PAD_BOOST) {
                NrrBoostSetEnv(b, 255, 223, 0, 255);
            } else {
                NrrBoostSetEnv(b, 91, 255, 91, 255);
            }
        } else {
            NrrBoostSetEnv(b, 0, 255, 255, 255);
        }
        scale = (racer->unk_17C - D_800F80A4) / (13.0f - D_800F80A4);
        for (i = info->count - 1; i >= 0; i--) {
            gEffectsVtxPtr = NrrBoostQuad(gEffectsVtxPtr, racer, info, i, flare, scale, &b->cam);
        }
        NrrBoostRecord(b, racer, info, start);
    }
}

/* The time-attack ghosts' flames (stock: no speed scale, magenta env at alpha 160). */
static void NrrBoostGhosts(NrrBoostBuild* b, s32 frame) {
    GhostRacer* ghost;
    if (gGameMode != GAMEMODE_TIME_ATTACK) {
        return;
    }
    NrrBoostSetEnv(b, 255, 0, 255, 160);
    for (ghost = &gGhostRacers[2]; ghost >= gGhostRacers; ghost--) {
        Racer* racer;
        BoosterInfo* info;
        Vtx* start = gEffectsVtxPtr;
        f32 flare;
        s32 i;
        if (!ghost->exists) {
            continue;
        }
        racer = ghost->racer;
        if (racer->unk_2B3 == 0) {
            continue;
        }
        info = &sBoosterInfos[racer->boostersType];
        flare = (f32) ((3 - (frame & 3)) << 1) * 0.2f;
        for (i = info->count - 1; i >= 0; i--) {
            gEffectsVtxPtr = NrrBoostQuad(gEffectsVtxPtr, racer, info, i, flare, 1.0f, &b->cam);
        }
        NrrBoostRecord(b, racer, info, start);
    }
}

/* ---- learn, tail --------------------------------------------------------------------------- */

static Gfx* NrrFxLearnMarker(Gfx* gfx, u32 op, u32 kind) {
    gfx->words.w0 = ((u32) G_NOOP << 24) | (GDX_NRRFX_LEARN_P << 16) | (op << 8) | (u32) NRR_CAT_FX;
    gfx->words.w1 = (GfxW1) kind;
    return gfx + 1;
}

static Gfx* NrrFxLearnPoint(Gfx* gfx, u32 kind) {
    gfx = NrrFxLearnMarker(gfx, GDX_NRRFX_OP_LEARN_BEGIN, kind);
    gSP2Triangles(gfx++, 0, 1, 2, 0, 0, 2, 1, 0);
    return NrrFxLearnMarker(gfx, GDX_NRRFX_OP_LEARN_END, kind);
}

/* Identity matrices around a learn sequence; the walker's model-view is pushed and restored. */
static Gfx* NrrFxLearnOpen(Gfx* gfx) {
    sLearns++;
    gSPMatrix(gfx++, &sIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(gfx++, &sIdentityMtx, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPVertex(gfx++, sLearnVtx, 3, 0);
    return gfx;
}

static Gfx* NrrFxLearnClose(Gfx* gfx, s32 view) {
    gSPPopMatrix(gfx++, G_MTX_MODELVIEW);
    gSPMatrix(gfx++, &D_1000000.unk_20208[view], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    return gfx;
}

/* One learn point when the kind is wanted and not learned. */
static Gfx* NrrFxLearnIf(Gfx* gfx, u32 kind, int want) {
    if (!want || NRRFX_READY[kind]) {
        return gfx;
    }
    return NrrFxLearnPoint(gfx, kind);
}

static int NrrFxNeedLearn(u32 lo, u32 hi) {
    u32 k;
    for (k = lo; k < hi; k++) {
        if (!NRRFX_READY[k]) {
            return 1;
        }
    }
    return 0;
}

/* The effects bracket's setup chain in stock order (each class inherits the state of the ones
 * before it, from the booster's list on), a learn point per kind not learned yet. */
static Gfx* NrrEfxWriteLearnChain(Gfx* gfx) {
    s32 i;
    gSPDisplayList(gfx++, D_4007FB8);
    for (i = 0; i < 8; i++) {
        if (NRRFX_READY[GDX_NRRFX_K_EXPL0 + i]) {
            continue;
        }
        gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255, 255);
        gDPSetEnvColor(gfx++, 255, 255, 0, 255);
        gDPLoadTextureBlock(gfx++, D_800CDAB8[i], G_IM_FMT_IA, G_IM_SIZ_8b, 32, 32, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, 0, 0, G_TX_NOLOD, G_TX_NOLOD);
        gfx = NrrFxLearnPoint(gfx, GDX_NRRFX_K_EXPL0 + i);
    }
    gSPDisplayList(gfx++, D_4007FD8);
    gfx = NrrFxLearnIf(gfx, GDX_NRRFX_K_DEBRIS, 1);
    gSPDisplayList(gfx++, D_4008000);
    gfx = NrrFxLearnIf(gfx, GDX_NRRFX_K_FSPARK, 1);
    gSPDisplayList(gfx++, D_4008028);
    for (i = 0; i < 8; i++) {
        if (!NRRFX_READY[GDX_NRRFX_K_CSPARK0 + i]) {
            gSPTexture(gfx++, 0x8000, 0x8000, 0, i, G_ON);
            gDPSetPrimColor(gfx++, 0, 0, 255, 255, 255, 255);
            gDPSetEnvColor(gfx++, 255, 255, 0, 255);
            gfx = NrrFxLearnPoint(gfx, GDX_NRRFX_K_CSPARK0 + i);
        }
    }
    gSPDisplayList(gfx++, D_4008130);
    for (i = 0; i < 8; i++) {
        if (!NRRFX_READY[GDX_NRRFX_K_SMOKE0 + i]) {
            gSPTexture(gfx++, 0x8000, 0x8000, 0, i, G_ON);
            gDPSetEnvColor(gfx++, 180, 150, 100, 230);
            gfx = NrrFxLearnPoint(gfx, GDX_NRRFX_K_SMOKE0 + i);
        }
    }
    gSPDisplayList(gfx++, aSetupPitForceFieldDL);
    for (i = 0; i < 8; i++) {
        if (!NRRFX_READY[GDX_NRRFX_K_PITFF0 + i]) {
            gSPTexture(gfx++, 0x8000, 0x8000, 0, i, G_ON);
            gfx = NrrFxLearnPoint(gfx, GDX_NRRFX_K_PITFF0 + i);
        }
    }
    return gfx;
}

/* Learn what is missing, under identity matrices: the booster's block, then (with the effects
 * part on) the effects chain -- before the marker, which draws both. */
static Gfx* NrrBoostWriteLearn(Gfx* gfx, s32 view, int boost) {
    int efx = (sParts & GDX_NRRFX_PART_EFX) && NrrFxNeedLearn(GDX_NRRFX_K_EXPL0, GDX_NRRFX_K_COUNT);
    if (!(boost && !NRRFX_READY[GDX_NRRFX_K_BOOST]) && !efx) {
        return gfx;
    }
    gfx = NrrFxLearnOpen(gfx);
    gSPDisplayList(gfx++, aSetupBoosterDL);
    gDPSetEnvColor(gfx++, 0, 255, 255, 255);
    gfx = NrrFxLearnIf(gfx, GDX_NRRFX_K_BOOST, boost);
    if (efx) {
        gfx = NrrEfxWriteLearnChain(gfx);
    }
    return NrrFxLearnClose(gfx, view);
}

static Gfx* NrrBoostWriteTail(Gfx* gfx, const NrrBoostBuild* b) {
    if (b->haveEnv) {
        gDPSetEnvColor(gfx++, b->env[0], b->env[1], b->env[2], b->env[3]);
    }
    return gfx;
}

/* ---- the stub ------------------------------------------------------------------------------- */

static int NrrFxNativeScope(s32 view) {
    if (!gdx3ds_nrr_view_ok(NRR_CAT_FX, view, gNumPlayers) || gdx3ds_nrr_fx_broken[1]) { /* niview: 2-4P views */
        return 0;
    }
#ifdef EXPANSION_KIT
    if (gGameMode == GAMEMODE_COURSE_EDIT) {
        return gdx3ds_c5tick_testdrive_scope(); /* S5b: the editor's test drive race, nothing else */
    }
#endif
    return gGameMode != GAMEMODE_GP_END_CS && gGameMode != GAMEMODE_COURSE_EDIT;
}

static void* NrrBoostStub(void* gfxv, int sec, int view) {
    Gfx* gfx = (Gfx*) gfxv;
    Camera* camera = &gCameras[view];
    NrrFrame* fr;
    NrrBoostBuild b;
    s32 frame;
    (void) sec;
    fr = NrrFxNativeScope(view) ? gdx3ds_nrr_frame_build() : NULL;
    memset(&b, 0, sizeof(b));
    b.snap = fr != NULL ? NrrFxSnapFor(fr, view) : NULL;
    if (b.snap != NULL && b.snap->boost == NULL) {
        b.snap->capBoost = TOTAL_RACER_COUNT + 3;
        if (gNumPlayers > 1) {
            b.snap->capBoost = (u16) ((sLastRacer - gRacers + 1) + 3); /* niview: the views share the arena */
        }
        b.snap->boost = (GdxNrrBoostEnt*) NrrSnapAlloc(fr, sizeof(GdxNrrBoostEnt) * b.snap->capBoost);
    }
    if (b.snap == NULL || b.snap->boost == NULL) {
        sStockFrames++;
        return NULL; /* out of scope, or the frame arena is full: the stock section builds */
    }
    b.cam.ux = camera->basis.y.x;
    b.cam.uy = camera->basis.y.y;
    b.cam.uz = camera->basis.y.z;
    b.cam.sx = camera->basis.z.x;
    b.cam.sy = camera->basis.z.y;
    b.cam.sz = camera->basis.z.z;
    frame = !gGamePaused ? (s32) gGameFrameCount : 1;
    NrrBoostRacers(&b, frame);
    NrrBoostGhosts(&b, frame);
    gfx = NrrBoostWriteLearn(gfx, view, b.snap->nBoost != 0);
    /* The stock setup list first: it loads the matrices the flames (and the effects after them)
     * are transformed with -- the marker reads the walker's MP. */
    gSPDisplayList(gfx++, aSetupBoosterDL);
    gfx = (Gfx*) gdx3ds_nrr_write_marker(gfx, NRR_CAT_FX, view, fr);
    b.snap->marked = 1;
    return NrrBoostWriteTail(gfx, &b);
}

/* ---- the effects: the stock vertex writes, recorded --------------------------------------- */

/* The camera axes (Racer_Draw: sp56C.. = basis.y, sp560.. = basis.z). */
typedef struct NrrEfxCam {
    f32 ux, uy, uz;
    f32 sx, sy, sz;
} NrrEfxCam;

/* The effects bracket's last register writes per class: what the stock commands leave set (the
 * tail re-asserts them after each class's setup list). */
typedef struct NrrEfxRegs {
    u8 prim[4];
    u8 env[4];
    s32 tile; /* -1: no gSPTexture since the setup list */
    s32 hasPrim, hasEnv;
} NrrEfxRegs;

typedef struct NrrEfxBuild {
    GdxNrrFxSprite* out;
    u32 n;
    u32 cap;
    NrrEfxCam cam;
    NrrEfxRegs regs[6]; /* expl, debris, fspark, cspark, smoke, pitff */
} NrrEfxBuild;

static GdxNrrFxSprite sEfxSprites[2][GDX_NRRFX_MAX_SPRITES];
/* niview: views 1-3 of a multi-view race, per pool, allocated at the first multi-view build. */
static GdxNrrFxSprite* sEfxSpritesView[2][NRR_MAX_VIEWS - 1];

static GdxNrrFxSprite* NrrEfxSpritesFor(s32 pool, s32 view) {
    GdxNrrFxSprite** slot;
    if (view <= 0) {
        return sEfxSprites[pool];
    }
    slot = &sEfxSpritesView[pool][view - 1];
    if (*slot == NULL) {
        *slot = (GdxNrrFxSprite*) gdx3ds_nrr_view_alloc(GDX_NRRFX_MAX_SPRITES * sizeof(GdxNrrFxSprite));
    }
    return *slot;
}

static void NrrEfxRegsReset(NrrEfxRegs* r) {
    memset(r, 0, sizeof(*r));
    r->tile = -1;
}

static void NrrEfxSetPrim(NrrEfxRegs* r, s32 cr, s32 cg, s32 cb, s32 ca) {
    r->prim[0] = (u8) cr;
    r->prim[1] = (u8) cg;
    r->prim[2] = (u8) cb;
    r->prim[3] = (u8) ca;
    r->hasPrim = 1;
}

static void NrrEfxSetEnv(NrrEfxRegs* r, s32 cr, s32 cg, s32 cb, s32 ca) {
    r->env[0] = (u8) cr;
    r->env[1] = (u8) cg;
    r->env[2] = (u8) cb;
    r->env[3] = (u8) ca;
    r->hasEnv = 1;
}

static void NrrEfxAdd(NrrEfxBuild* b, const Vtx* v, u32 kind, u32 pat, const NrrEfxRegs* r) {
    GdxNrrFxSprite* e;
    if (b->n >= b->cap) {
        sOverflow++;
        return;
    }
    e = &b->out[b->n++];
    e->vtx = v;
    e->kind = (u8) kind;
    e->pat = (u8) pat;
    e->set = (u8) ((r->hasPrim ? GDX_NRRFX_SET_PRIM : 0) | (r->hasEnv ? GDX_NRRFX_SET_ENV : 0));
    memcpy(e->prim, r->prim, 4);
    memcpy(e->env, r->env, 4);
}

/* A camera-facing quad of half-size `scale` at (x, y, z): Racer_Draw's explosion / spark / smoke
 * vertex expressions and texture coordinates (0 / hi). */
static void NrrEfxBillboard(Vtx* v, f32 x, f32 y, f32 z, f32 scale, const NrrEfxCam* c, s16 hi) {
    f32 u0 = scale * c->ux;
    f32 u1 = scale * c->uy;
    f32 u2 = scale * c->uz;
    f32 s0 = scale * c->sx;
    f32 s1 = scale * c->sy;
    f32 s2 = scale * c->sz;
    v[0].v.ob[0] = Math_Round(x + u0);
    v[0].v.ob[1] = Math_Round(y + u1);
    v[0].v.ob[2] = Math_Round(z + u2);
    v[2].v.ob[0] = Math_Round(x - u0);
    v[2].v.ob[1] = Math_Round(y - u1);
    v[2].v.ob[2] = Math_Round(z - u2);
    v[1].v.ob[0] = Math_Round(x + s0);
    v[1].v.ob[1] = Math_Round(y + s1);
    v[1].v.ob[2] = Math_Round(z + s2);
    v[3].v.ob[0] = Math_Round(x - s0);
    v[3].v.ob[1] = Math_Round(y - s1);
    v[3].v.ob[2] = Math_Round(z - s2);
    v[0].v.tc[0] = v[1].v.tc[0] = v[1].v.tc[1] = v[2].v.tc[1] = 0;
    v[0].v.tc[1] = v[2].v.tc[0] = v[3].v.tc[0] = v[3].v.tc[1] = hi;
}

static void NrrEfxExplosions1(NrrEfxBuild* b) {
    NrrEfxRegs* r = &b->regs[0];
    s32 n;
    s32 i;
    for (n = gExplosions1Count, i = (gExplosions1Index - 1) & 0x1F; (n != 0) && (gEffectsVtxPtr <= (gEffectsVtxEndPtr - 3));
         i = (i - 1) & 0x1F, n--) {
        MachineEffect* e = &gExplosions1[i];
        NrrEfxBillboard(gEffectsVtxPtr, e->pos.x, e->pos.y, e->pos.z, e->scale, &b->cam, 0x7FF);
        if (e->timer < 8) {
            NrrEfxSetPrim(r, 255, 255, 255 - (e->timer * 16), 255);
            NrrEfxSetEnv(r, 255, 255 - (e->timer * 32), 0, 255);
        } else {
            NrrEfxSetPrim(r, 255, 255, 255 - (e->timer * 16), 255);
            NrrEfxSetEnv(r, 0x1FF - (e->timer * 32), 0, 0, 0x17F - (e->timer * 16));
        }
        NrrEfxAdd(b, gEffectsVtxPtr, GDX_NRRFX_K_EXPL0 + ((e->timer >> 1) & 7), GDX_NRRFX_PAT_QUAD, r);
        gEffectsVtxPtr += 4;
    }
}

static void NrrEfxExplosions2(NrrEfxBuild* b) {
    NrrEfxRegs* r = &b->regs[0];
    s32 n;
    s32 i;
    for (n = gExplosions2Count, i = (gExplosions2Index - 1) & 7; (n != 0) && (gEffectsVtxPtr <= (gEffectsVtxEndPtr - 3));
         i = (i - 1) & 7, n--) {
        MachineEffect* e = &gExplosions2[i];
        NrrEfxBillboard(gEffectsVtxPtr, e->pos.x, e->pos.y, e->pos.z, e->scale, &b->cam, 0x7FF);
        if (e->timer < 16) {
            NrrEfxSetPrim(r, 255, 255, 255 - (e->timer * 8), 255);
            NrrEfxSetEnv(r, 255, 255 - (e->timer * 16), 0, 255);
        } else {
            NrrEfxSetPrim(r, 255, 255, 255 - (e->timer * 8), 255);
            NrrEfxSetEnv(r, 511 - (e->timer * 16), 0, 0, 383 - (e->timer * 8));
        }
        NrrEfxAdd(b, gEffectsVtxPtr, GDX_NRRFX_K_EXPL0 + ((e->timer >> 2) & 7), GDX_NRRFX_PAT_QUAD, r);
        gEffectsVtxPtr += 4;
    }
}

static void NrrEfxDebris(NrrEfxBuild* b) {
    s32 n;
    s32 i;
    for (n = gMachineDebrisCount, i = (gMachineDebrisIndex - 1) & 0x7F;
         (n != 0) && (gEffectsVtxPtr <= (gEffectsVtxEndPtr - 2)); i = (i - 1) & 0x7F, n--) {
        MachineDebrisEffect* d = &gMachineDebris[i];
        Vtx* v = gEffectsVtxPtr;
        v[0].v.ob[0] = d->cornerPos1.x;
        v[0].v.ob[1] = d->cornerPos1.y;
        v[0].v.ob[2] = d->cornerPos1.z;
        v[1].v.ob[0] = d->cornerPos2.x;
        v[1].v.ob[1] = d->cornerPos2.y;
        v[1].v.ob[2] = d->cornerPos2.z;
        v[2].v.ob[0] = d->cornerPos3.x;
        v[2].v.ob[1] = d->cornerPos3.y;
        v[2].v.ob[2] = d->cornerPos3.z;
        v[0].v.cn[0] = d->red;
        v[0].v.cn[1] = d->green;
        v[0].v.cn[2] = d->blue;
        v[0].v.cn[3] = d->alpha;
        NrrEfxAdd(b, v, GDX_NRRFX_K_DEBRIS, GDX_NRRFX_PAT_TRI, &b->regs[1]);
        gEffectsVtxPtr += 3;
    }
}

static void NrrEfxFlyingSparks(NrrEfxBuild* b) {
    s32 n;
    s32 i;
    for (n = gFlyingSparksCount, i = (gFlyingSparksIndex - 1) & 0x1F;
         (n != 0) && (gEffectsVtxPtr <= (gEffectsVtxEndPtr - 2)); i = (i - 1) & 0x1F, n--) {
        FlyingSparkEffect* f = &gFlyingSparks[i];
        Vtx* v = gEffectsVtxPtr;
        v[0].v.ob[0] = Math_Round(f->pos.x - (3.0f * f->velocity.x));
        v[0].v.ob[1] = Math_Round(f->pos.y - (3.0f * f->velocity.y));
        v[0].v.ob[2] = Math_Round(f->pos.z - (3.0f * f->velocity.z));
        v[1].v.ob[0] = Math_Round(f->pos.x + f->basis.z.x);
        v[1].v.ob[1] = Math_Round(f->pos.y + f->basis.z.y);
        v[1].v.ob[2] = Math_Round(f->pos.z + f->basis.z.z);
        v[2].v.ob[0] = Math_Round(f->pos.x - f->basis.z.x);
        v[2].v.ob[1] = Math_Round(f->pos.y - f->basis.z.y);
        v[2].v.ob[2] = Math_Round(f->pos.z - f->basis.z.z);
        v[0].v.cn[0] = 0xFF;
        v[0].v.cn[1] = v[0].v.cn[2] = 0;
        v[0].v.cn[3] = 0;
        v[1].v.cn[0] = v[2].v.cn[0] = v[1].v.cn[1] = v[2].v.cn[1] = 0xFF;
        v[1].v.cn[2] = v[2].v.cn[2] = 0;
        v[1].v.cn[3] = v[2].v.cn[3] = 0xFF;
        NrrEfxAdd(b, v, GDX_NRRFX_K_FSPARK, GDX_NRRFX_PAT_TRI, &b->regs[2]);
        gEffectsVtxPtr += 3;
    }
}

static void NrrEfxCollisionSparks(NrrEfxBuild* b) {
    NrrEfxRegs* r = &b->regs[3];
    s32 n;
    s32 i;
    for (n = gCollisionSparkCount, i = (gCollisionSparkIndex - 1) & 0x1F;
         (n != 0) && (gEffectsVtxPtr <= (gEffectsVtxEndPtr - 3)); i = (i - 1) & 0x1F, n--) {
        MachineEffect* e = &gCollisionSparks[i];
        s32 t;
        f32 drop;
        f32 x;
        f32 y;
        f32 z;
        if (e->racer->unk_2B3 == 0) {
            continue;
        }
        t = e->timer - 1;
        drop = SQ(t) * -0.15f;
        x = e->pos.x + (drop * e->racer->trueBasis.y.x);
        y = e->pos.y + (drop * e->racer->trueBasis.y.y);
        z = e->pos.z + (drop * e->racer->trueBasis.y.z);
        NrrEfxBillboard(gEffectsVtxPtr, x, y, z, e->scale, &b->cam, 0x7FF);
        r->tile = e->timer >> 1;
        NrrEfxSetPrim(r, 255, 255, 255 - (e->timer * 8), 255);
        NrrEfxSetEnv(r, 255, 255 - (e->timer * 16), 0, 255 - (e->timer * 4));
        NrrEfxAdd(b, gEffectsVtxPtr, GDX_NRRFX_K_CSPARK0 + (r->tile & 7), GDX_NRRFX_PAT_QUAD, r);
        gEffectsVtxPtr += 4;
    }
}

static void NrrEfxSmoke(NrrEfxBuild* b) {
    NrrEfxRegs* r = &b->regs[4];
    s32 n;
    s32 i;
    for (n = gSmokesCount, i = (gSmokesIndex - 1) & 0x1F; (n != 0) && (gEffectsVtxPtr <= (gEffectsVtxEndPtr - 3));
         i = (i - 1) & 0x1F, n--) {
        MachineEffect* e = &gSmokes[i];
        if (e->racer->unk_2B3 == 0) {
            continue;
        }
        NrrEfxBillboard(gEffectsVtxPtr, e->pos.x, e->pos.y, e->pos.z, e->scale, &b->cam, 0x7FF);
        r->tile = e->timer >> 2;
        NrrEfxSetEnv(r, 180, 150, 100, 230 - (e->timer * 4));
        NrrEfxAdd(b, gEffectsVtxPtr, GDX_NRRFX_K_SMOKE0 + (r->tile & 7), GDX_NRRFX_PAT_QUAD, r);
        gEffectsVtxPtr += 4;
    }
}

/* The pit force field: up and side half-sizes differ (38 / 40), texture coordinates 0x20 / 0x7E0. */
static void NrrEfxPitForceField(NrrEfxBuild* b) {
    NrrEfxRegs* r = &b->regs[5];
    Racer* racer;
    s32 tile = !gGamePaused ? (s32) (gGameFrameCount & 7) : 0;
    for (racer = sLastRacer; (racer >= gRacers) && (gEffectsVtxPtr <= (gEffectsVtxEndPtr - 3)); racer--) {
        Vtx* v = gEffectsVtxPtr;
        f32 h;
        f32 x;
        f32 y;
        f32 z;
        f32 u0;
        f32 u1;
        f32 u2;
        f32 s0;
        f32 s1;
        f32 s2;
        if ((racer->pitForceFieldSize == 0.0f) || (racer->unk_2B3 == 0)) {
            continue;
        }
        h = 3.0f - ((1.0f - racer->pitForceFieldSize) * 19.0f);
        x = racer->segmentPositionInfo.pos.x + (h * racer->trueBasis.y.x);
        y = racer->segmentPositionInfo.pos.y + (h * racer->trueBasis.y.y);
        z = racer->segmentPositionInfo.pos.z + (h * racer->trueBasis.y.z);
        h = racer->pitForceFieldSize * 38.0f;
        u0 = h * b->cam.ux;
        u1 = h * b->cam.uy;
        u2 = h * b->cam.uz;
        h = racer->pitForceFieldSize * 40.0f;
        v[0].v.ob[0] = Math_Round(x + u0);
        v[0].v.ob[1] = Math_Round(y + u1);
        v[0].v.ob[2] = Math_Round(z + u2);
        v[2].v.ob[0] = Math_Round(x - u0);
        v[2].v.ob[1] = Math_Round(y - u1);
        v[2].v.ob[2] = Math_Round(z - u2);
        s0 = h * b->cam.sx;
        v[1].v.ob[0] = Math_Round(x + s0);
        s1 = h * b->cam.sy;
        v[1].v.ob[1] = Math_Round(y + s1);
        s2 = h * b->cam.sz;
        v[1].v.ob[2] = Math_Round(z + s2);
        v[3].v.ob[0] = Math_Round(x - s0);
        v[3].v.ob[1] = Math_Round(y - s1);
        v[3].v.ob[2] = Math_Round(z - s2);
        v[2].v.tc[0] = v[3].v.tc[0] = v[3].v.tc[1] = v[0].v.tc[1] = 0x20;
        v[2].v.tc[1] = v[0].v.tc[0] = v[1].v.tc[0] = v[1].v.tc[1] = 0x7E0;
        r->tile = tile;
        NrrEfxAdd(b, v, GDX_NRRFX_K_PITFF0 + tile, GDX_NRRFX_PAT_QUAD, r);
        gEffectsVtxPtr += 4;
    }
}

/* The fall explosions stay stock commands (a two-texture combiner): Racer_Draw's loop verbatim,
 * after the marker, so the order of the stream is the stock one. */
static Gfx* NrrEfxFallExplosions(Gfx* gfx, const NrrEfxCam* c) {
    s32 n;
    s32 i;
    gSPDisplayList(gfx++, aSetupFallExplosionDL);
    for (n = gFallExplosionsCount, i = (gFallExplosionsIndex - 1) & 0x1F;
         (n != 0) && (gEffectsVtxPtr <= (gEffectsVtxEndPtr - 7)); i = (i - 1) & 0x1F, n--) {
        FallExplosionEffect* fx = &gFallExplosions[i];
        Racer* racer = fx->racer;
        s32 t = fx->timer - 5;
        f32 s;
        f32 a0;
        f32 a1;
        f32 a2;
        f32 b0;
        f32 b1;
        f32 b2;
        s32 fade;
        if (fx->timer == 0) {
            continue;
        }
        if ((t >= 0) && (t < 60)) {
            if (gEffectsVtxPtr > (gEffectsVtxEndPtr - 3)) {
                break;
            }
            s = SIN(((s32) (t << 10) / 60)) * 163.64f;
            if (t < 20) {
                fade = 255;
            } else {
                fade = (s32) ((60 - t) * 255) / 40;
            }
            s *= fx->scale;
            a0 = s * c->ux;
            gEffectsVtxPtr[0].v.ob[0] = Math_Round(racer->segmentPositionInfo.pos.x + a0);
            a1 = s * c->uy;
            gEffectsVtxPtr[0].v.ob[1] = Math_Round(racer->segmentPositionInfo.pos.y + a1);
            a2 = s * c->uz;
            gEffectsVtxPtr[0].v.ob[2] = Math_Round(racer->segmentPositionInfo.pos.z + a2);
            gEffectsVtxPtr[2].v.ob[0] = Math_Round(racer->segmentPositionInfo.pos.x - a0);
            gEffectsVtxPtr[2].v.ob[1] = Math_Round(racer->segmentPositionInfo.pos.y - a1);
            gEffectsVtxPtr[2].v.ob[2] = Math_Round(racer->segmentPositionInfo.pos.z - a2);
            a0 = s * c->sx;
            gEffectsVtxPtr[1].v.ob[0] = Math_Round(racer->segmentPositionInfo.pos.x + a0);
            a1 = s * c->sy;
            gEffectsVtxPtr[1].v.ob[1] = Math_Round(racer->segmentPositionInfo.pos.y + a1);
            a2 = s * c->sz;
            gEffectsVtxPtr[1].v.ob[2] = Math_Round(racer->segmentPositionInfo.pos.z + a2);
            gEffectsVtxPtr[3].v.ob[0] = Math_Round(racer->segmentPositionInfo.pos.x - a0);
            gEffectsVtxPtr[3].v.ob[1] = Math_Round(racer->segmentPositionInfo.pos.y - a1);
            gEffectsVtxPtr[3].v.ob[2] = Math_Round(racer->segmentPositionInfo.pos.z - a2);
            gEffectsVtxPtr[0].v.tc[0] = gEffectsVtxPtr[1].v.tc[0] = gEffectsVtxPtr[1].v.tc[1] =
                gEffectsVtxPtr[2].v.tc[1] = 0x40;
            gEffectsVtxPtr[0].v.tc[1] = gEffectsVtxPtr[2].v.tc[0] = gEffectsVtxPtr[3].v.tc[0] =
                gEffectsVtxPtr[3].v.tc[1] = 0x1FBF;
            gSPTexture(gfx++, 0x8000, 0x8000, 0, 0, G_ON);
            gSPVertex(gfx++, gEffectsVtxPtr, 4, 0);
            gDPPipeSync(gfx++);
            gDPSetPrimColor(gfx++, 0, 0, 255, fade, 80, 255);
            gDPSetEnvColor(gfx++, fade, 0, 0, fade);
            gSP2Triangles(gfx++, 0, 1, 3, 0, 1, 2, 3, 0);
            gEffectsVtxPtr += 4;
        }
        if (fx->timer < 26) {
            if (gEffectsVtxPtr > (gEffectsVtxEndPtr - 3)) {
                break;
            }
            s = (fx->scale * 163.64f * (f32) fx->timer) / 26.0f;
            a0 = racer->trueBasis.x.x * s;
            a1 = racer->trueBasis.x.y * s;
            a2 = racer->trueBasis.x.z * s;
            b0 = racer->trueBasis.z.x * s;
            b1 = racer->trueBasis.z.y * s;
            b2 = racer->trueBasis.z.z * s;
            gEffectsVtxPtr[0].v.ob[0] = Math_Round(racer->segmentPositionInfo.pos.x + a0);
            gEffectsVtxPtr[0].v.ob[1] = Math_Round(racer->segmentPositionInfo.pos.y + a1);
            gEffectsVtxPtr[0].v.ob[2] = Math_Round(racer->segmentPositionInfo.pos.z + a2);
            gEffectsVtxPtr[2].v.ob[0] = Math_Round(racer->segmentPositionInfo.pos.x - a0);
            gEffectsVtxPtr[2].v.ob[1] = Math_Round(racer->segmentPositionInfo.pos.y - a1);
            gEffectsVtxPtr[2].v.ob[2] = Math_Round(racer->segmentPositionInfo.pos.z - a2);
            gEffectsVtxPtr[1].v.ob[0] = Math_Round(racer->segmentPositionInfo.pos.x + b0);
            gEffectsVtxPtr[1].v.ob[1] = Math_Round(racer->segmentPositionInfo.pos.y + b1);
            gEffectsVtxPtr[1].v.ob[2] = Math_Round(racer->segmentPositionInfo.pos.z + b2);
            gEffectsVtxPtr[3].v.ob[0] = Math_Round(racer->segmentPositionInfo.pos.x - b0);
            gEffectsVtxPtr[3].v.ob[1] = Math_Round(racer->segmentPositionInfo.pos.y - b1);
            gEffectsVtxPtr[3].v.ob[2] = Math_Round(racer->segmentPositionInfo.pos.z - b2);
            gEffectsVtxPtr[2].v.tc[1] = 0x40;
            gEffectsVtxPtr[0].v.tc[0] = gEffectsVtxPtr[1].v.tc[0] = gEffectsVtxPtr[1].v.tc[1] =
                gEffectsVtxPtr[2].v.tc[1];
            gEffectsVtxPtr[3].v.tc[1] = 0x1FBF;
            gEffectsVtxPtr[0].v.tc[1] = gEffectsVtxPtr[2].v.tc[0] = gEffectsVtxPtr[3].v.tc[0] =
                gEffectsVtxPtr[3].v.tc[1];
            gSPTexture(gfx++, 0x8000, 0x8000, 0, 1, G_ON);
            gSPVertex(gfx++, gEffectsVtxPtr, 4, 0);
            gDPPipeSync(gfx++);
            gDPSetPrimColor(gfx++, 0, 0, 100, 255, 255, 255);
            gDPSetEnvColor(gfx++, 0, 0, 255, (((26 - fx->timer) * 255) / 26));
            gSP2Triangles(gfx++, 0, 1, 3, 0, 1, 2, 3, 0);
            gEffectsVtxPtr += 4;
        }
    }
    return gfx;
}

/* One class's tail: its setup list and the last register values its sprites set. */
static Gfx* NrrEfxTailClass(Gfx* gfx, Gfx* setup, const NrrEfxRegs* r) {
    gSPDisplayList(gfx++, setup);
    if (r->tile >= 0) {
        gSPTexture(gfx++, 0x8000, 0x8000, 0, r->tile, G_ON);
    }
    if (r->hasPrim) {
        gDPSetPrimColor(gfx++, 0, 0, r->prim[0], r->prim[1], r->prim[2], r->prim[3]);
    }
    if (r->hasEnv) {
        gDPSetEnvColor(gfx++, r->env[0], r->env[1], r->env[2], r->env[3]);
    }
    return gfx;
}

/* What the stock bracket leaves set before its fall explosions: every class's setup list in order,
 * each followed by the last registers its sprites wrote (texture loads are re-done by the later
 * classes' setup lists). */
static Gfx* NrrEfxWriteTail(Gfx* gfx, const NrrEfxBuild* b) {
    gfx = NrrEfxTailClass(gfx, D_4007FB8, &b->regs[0]);
    gfx = NrrEfxTailClass(gfx, D_4007FD8, &b->regs[1]);
    gfx = NrrEfxTailClass(gfx, D_4008000, &b->regs[2]);
    gfx = NrrEfxTailClass(gfx, D_4008028, &b->regs[3]);
    gfx = NrrEfxTailClass(gfx, D_4008130, &b->regs[4]);
    return NrrEfxTailClass(gfx, aSetupPitForceFieldDL, &b->regs[5]);
}

static void* NrrEfxStub(void* gfxv, int sec, int view) {
    Gfx* gfx = (Gfx*) gfxv;
    Camera* camera = &gCameras[view];
    NrrFrame* fr;
    GdxNrrFxSnap* snap;
    NrrEfxBuild b;
    s32 pool = (s32) (gGfxPool - D_8024DCE0);
    s32 k;
    (void) sec;
    fr = (NrrFxNativeScope(view) && pool >= 0 && pool < 2) ? gdx3ds_nrr_frame_build() : NULL;
    snap = fr != NULL ? NrrFxSnapFor(fr, view) : NULL;
    if (snap == NULL) {
        sStockFrames++;
        return NULL;
    }
    memset(&b, 0, sizeof(b));
    b.out = NrrEfxSpritesFor(pool, view);
    if (b.out == NULL) {
        sStockFrames++;
        return NULL; /* no memory for this view's sprite records: the stock section builds */
    }
    b.cap = GDX_NRRFX_MAX_SPRITES;
    for (k = 0; k < 6; k++) {
        NrrEfxRegsReset(&b.regs[k]);
    }
    b.cam.ux = camera->basis.y.x;
    b.cam.uy = camera->basis.y.y;
    b.cam.uz = camera->basis.y.z;
    b.cam.sx = camera->basis.z.x;
    b.cam.sy = camera->basis.z.y;
    b.cam.sz = camera->basis.z.z;
    NrrEfxExplosions1(&b);
    NrrEfxExplosions2(&b);
    NrrEfxDebris(&b);
    NrrEfxFlyingSparks(&b);
    NrrEfxCollisionSparks(&b);
    NrrEfxSmoke(&b);
    NrrEfxPitForceField(&b);
    snap->sprites = b.out;
    snap->nSprites = (u16) b.n;
    if (!snap->marked) {
        if (NrrFxNeedLearn(GDX_NRRFX_K_EXPL0, GDX_NRRFX_K_COUNT)) {
            gfx = NrrFxLearnOpen(gfx); /* the stock booster commands just ran: their state is inherited */
            gfx = NrrEfxWriteLearnChain(gfx);
            gfx = NrrFxLearnClose(gfx, view);
        }
        gfx = (Gfx*) gdx3ds_nrr_write_marker(gfx, NRR_CAT_FX, view, fr);
        snap->marked = 1;
    }
    if (sTail) {
        gfx = NrrEfxWriteTail(gfx, &b);
    }
    return NrrEfxFallExplosions(gfx, &b.cam);
}

/* ---- the category init (core contract: called once at boot when nrr_fx is on) -------------- */

/* n60stereo (gdx3ds_stereo_frame.cpp): 1 = the NRR may draw under the [stereo] config. */
int gdx3ds_stereo_nrr_ok(void) __attribute__((weak));

void gdx3ds_nrr_cat_init_fx(void) {
    int taken = 0;
    guMtxIdent(&sIdentityMtx);
    if (&gdx3ds_stereo_nrr_ok != NULL && !gdx3ds_stereo_nrr_ok()) {
        /* [stereo] enabled with [native] stereo_replay=0: the core draws mono only
         * (gdx3ds_nrr_bk_frame_active), a consumed section would vanish from a stereo frame, so the
         * section stays stock (as nrr_mach). With the frame replay (n60stereo) it draws both eyes. */
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[nrr] %s init REFUSED: [stereo] enabled, stereo_replay=0 (mono only); the section stays "
                         "stock\n", "fx");
        }
        return;
    }
    sParts = &gdx3ds_config_get_int != NULL ? gdx3ds_config_get_int("native", "nrr_fx_parts", 3) : 3;
    sTail = &gdx3ds_config_get_int != NULL ? gdx3ds_config_get_int("native", "nrr_fx_tail", 0) : 0;
    gdx3ds_nrr_fx_render_init();
    if (sParts & GDX_NRRFX_PART_BOOST) {
        taken |= gdx3ds_c5tick_consume(GDX_DLB_SEC_BOOST, NrrBoostStub);
    }
    if (sParts & GDX_NRRFX_PART_EFX) {
        taken |= gdx3ds_c5tick_consume(GDX_DLB_SEC_EFX, NrrEfxStub) << 1;
    }
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[nrr] fx init parts=%d consumed=%d tail=%d (1P race: learned sprite blocks, SpTri's clipper; 4P / "
                     "GP ending / editor stock)\n",
                     sParts, taken, sTail);
    }
}

/* [nrrfx] receipt counters (game side): learn sequences written, stock-built sections, entries
 * that did not fit the snapshot. */
void gdx3ds_nrr_fx_game_counts(unsigned long out[3]) {
    out[0] = sLearns;
    out[1] = sStockFrames;
    out[2] = sOverflow;
}
