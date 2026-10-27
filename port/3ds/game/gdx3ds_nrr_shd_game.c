/* port/3ds/game/gdx3ds_nrr_shd_game.c -- [native] nrr_shd: the game side of the machine shadows on
 * the NRR (gfx/nrr/gdx3ds_nrr_fx.h, docs/research/native/n60fx-progress.md).
 *
 * Racer_Draw's SHD section is consumed (gdx3ds_c5tick_consume; c5tick-progress.md §3.1: its only
 * game-state writes are racer->shadowMatrixInit, which the racer update resets every tick, and the
 * intro speed clamp, which the decomp patch keeps on the skip path). This stub makes the section's
 * exact writes -- every drawn racer's shadow matrix into the GfxPool and shadowMatrixInit -- and, in
 * place of its ~7 commands + one texture load per shadow, writes:
 *
 *   [learn]  when a shadow texture it needs has no learned block: the stock setup list, then per
 *            kind the stock texture load and prim colour with a dummy triangle between LEARN
 *            markers, under identity matrices; then the stock projection again;
 *   marker   one core marker over the snapshot (per shadow: MP, vertices, kind, prim);
 *   tail     the stock section's final state: its setup list and the last shadow's matrix
 *            (the walker's model-view after the section is what the stock one leaves).
 *
 * Scope: the 1P race outside the GP ending (the ending's XLU decal render mode is not learned) and
 * the course editor; everything else gets the stock section (the stub returns NULL).
 * Compiled into gdx3ds_game (decomp include paths and types). */
#include "global.h"
#include "fzx_racer.h"
#include "fzx_game.h"
#include "fzx_math.h"
#include ASSET_HEADER(machine_custom_gfx.h)
#include ASSET_HEADER(machine_global_gfx.h)

#include "nrr/gdx3ds_nrr.h"
#include "nrr/gdx3ds_nrr_fx.h"

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
typedef void* (*GdxDlbStubFn)(void* gfx, int sec, int view);
extern int gdx3ds_c5tick_consume(int sec, GdxDlbStubFn stub);
#ifdef EXPANSION_KIT
extern int gdx3ds_c5tick_testdrive_scope(void); /* S5b: gdx3ds_c5tick.h */
#endif

extern GfxPool* gGfxPool;
extern GfxPool D_1000000;
extern Racer* sLastRacer;
extern GhostRacer gGhostRacers[3];
extern Vtx* D_800CDBA4[];
extern TexturePtr D_800CDC54[];
extern f32 D_800CE748;
extern f32 D_800CE750;
extern s32 gRaceIntroTimer;
extern s32 gGameMode;
extern s32 gNumPlayers;

#define NRRSHD_READY gdx3ds_nrr_fx_ready[0]

static Mtx sIdentityMtx;
static Vtx sLearnVtx[3] = {
    { { { 0, 0, 0 }, 0, { 0, 0 }, { 255, 255, 255, 255 } } },
    { { { 1, 0, 0 }, 0, { 32, 0 }, { 255, 255, 255, 255 } } },
    { { { 0, 1, 0 }, 0, { 0, 32 }, { 255, 255, 255, 255 } } },
};
static int sTail = 0; /* [native] nrr_shd_tail (default 0, lean): 1 = D_303A758 + the last shadow matrix after the marker;
                         0 = the matrix only */
static u32 sLearns;
static u32 sStockFrames;
static u32 sOverflow;

/* ---- math (== n64/gdx_rsp.cpp SpMatrix's fixed-point load and MatrixMul) -------------------- */

static void NrrShdMtxToFloat(const Mtx* m, f32 out[4][4]) {
    const s32* w = (const s32*) m;
    s32 i;
    for (i = 0; i < 8; i++) {
        s32 ip = w[i];
        u32 fp = (u32) w[8 + i];
        out[i / 2][(i & 1) * 2] = (s32) ((ip & 0xffff0000) | (fp >> 16)) / 65536.0f;
        out[i / 2][(i & 1) * 2 + 1] = (s32) ((ip << 16) | (fp & 0xffff)) / 65536.0f;
    }
}

static void NrrShdMatMul(f32 res[4][4], f32 a[4][4], f32 b[4][4]) {
    s32 r;
    s32 k;
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 4; k++) {
            res[r][k] = a[r][0] * b[0][k] + a[r][1] * b[1][k] + a[r][2] * b[2][k] + a[r][3] * b[3][k];
        }
    }
}

/* ---- the section's game-state writes (stock expressions) ------------------------------------ */

static int NrrShdDrawn(const Racer* racer) {
    return racer->unk_2B2 != 0 && racer->shadowPos.y != -54321.0f;
}

static void NrrShdRacerMatrix(Racer* racer) {
    f32 scaleX;
    if (racer->shadowMatrixInit) {
        return;
    }
    if (racer->attackState == ATTACK_STATE_SIDE) {
        scaleX = ((COS(racer->unk_27C) * 0.15f) + 0.85f) * D_800CE750;
    } else {
        scaleX = D_800CE750;
    }
    Matrix_SetLockedLookAtFromVectors(&gGfxPool->unk_20A88[racer->id], NULL, racer->shadowColorStrength * D_800CE748,
                                      0.1f, racer->shadowColorStrength * scaleX, &racer->modelBasis.x,
                                      &racer->upFromGround, &racer->shadowPos);
    racer->shadowMatrixInit = true;
}

/* The ghost's matrix is rebuilt every frame by the stock section (no init flag). */
static void NrrShdGhostMatrix(GhostRacer* ghost) {
    Racer* racer = ghost->racer;
    Matrix_SetLockedLookAtFromVectors(&gGfxPool->unk_20A88[racer->id], NULL, ghost->scale * D_800CE748, 0.1f,
                                      ghost->scale * D_800CE750, &racer->modelBasis.x, &racer->upFromGround,
                                      &racer->shadowPos);
}

/* ---- snapshot ------------------------------------------------------------------------------ */

typedef struct NrrShdBuild {
    GdxNrrShdSnap* snap;
    u32 cap;
    f32 proj[4][4];
    s32 lastId;       /* the racer whose matrix the stock section loads last; -1 none */
    u8 learn[GDX_NRRSHD_KINDS];
    u8 needLearn;
} NrrShdBuild;

static void NrrShdAdd(NrrShdBuild* b, Racer* racer, s32 kind, const void* vtx, s32 r, s32 g, s32 bl) {
    GdxNrrShdEnt* e;
    f32 mv[4][4];
    b->lastId = racer->id;
    if (!NRRSHD_READY[kind] && !b->learn[kind]) {
        b->learn[kind] = 1;
        b->needLearn = 1;
    }
    if (b->snap->n >= b->cap) {
        sOverflow++;
        return;
    }
    e = &b->snap->e[b->snap->n++];
    NrrShdMtxToFloat(&gGfxPool->unk_20A88[racer->id], mv);
    NrrShdMatMul(e->mp, mv, b->proj);
    e->vtx = vtx;
    e->kind = (u8) kind;
    e->prim[0] = (u8) r;
    e->prim[1] = (u8) g;
    e->prim[2] = (u8) bl;
    e->prim[3] = 200;
}

static void NrrShdCollectRacers(NrrShdBuild* b) {
    Racer* racer;
    for (racer = sLastRacer; racer >= gRacers; racer--) {
        if (!NrrShdDrawn(racer)) {
            continue;
        }
        NrrShdRacerMatrix(racer);
        if (!(racer->stateFlags & RACER_STATE_CRASHED)) {
            NrrShdAdd(b, racer, racer->shadowType, D_800CDBA4[racer->shadowType], racer->shadowR, racer->shadowG,
                      racer->shadowB);
        } else {
            NrrShdAdd(b, racer, GDX_NRRSHD_KIND_CRASHED, D_7048868, 0, 0, 0);
        }
    }
}

static void NrrShdCollectGhosts(NrrShdBuild* b) {
    GhostRacer* ghost;
    if (gGameMode != GAMEMODE_TIME_ATTACK) {
        return;
    }
    for (ghost = &gGhostRacers[2]; ghost >= gGhostRacers; ghost--) {
        if (!ghost->exists || !NrrShdDrawn(ghost->racer)) {
            continue;
        }
        NrrShdGhostMatrix(ghost);
        NrrShdAdd(b, ghost->racer, ghost->racer->shadowType, D_800CDBA4[ghost->racer->shadowType], 0, 0, 0);
    }
}

/* ---- learn sequence: the stock state commands, one learn point per kind ------------------- */

static Gfx* NrrShdLearnMarker(Gfx* gfx, u32 op, u32 kind) {
    gfx->words.w0 = ((u32) G_NOOP << 24) | (GDX_NRRFX_LEARN_P << 16) | (op << 8) | (u32) NRR_CAT_SHD;
    gfx->words.w1 = (GfxW1) kind;
    return gfx + 1;
}

static Gfx* NrrShdLearnPoint(Gfx* gfx, u32 kind) {
    if (kind == GDX_NRRSHD_KIND_CRASHED) {
        gDPLoadTextureBlock_4b(gfx++, D_70488A8, G_IM_FMT_I, 32, 64, 0, G_TX_MIRROR | G_TX_CLAMP,
                               G_TX_MIRROR | G_TX_CLAMP, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
    } else {
        gDPLoadTextureBlock_4b(gfx++, D_800CDC54[kind], G_IM_FMT_I, 32, 64, 0, G_TX_MIRROR | G_TX_CLAMP,
                               G_TX_MIRROR | G_TX_CLAMP, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
    }
    gDPSetPrimColor(gfx++, 0, 0, 0, 0, 0, 200);
    gfx = NrrShdLearnMarker(gfx, GDX_NRRFX_OP_LEARN_BEGIN, kind);
    gSP2Triangles(gfx++, 0, 1, 2, 0, 0, 2, 1, 0);
    return NrrShdLearnMarker(gfx, GDX_NRRFX_OP_LEARN_END, kind);
}

static Gfx* NrrShdWriteLearn(Gfx* gfx, const NrrShdBuild* b, s32 view) {
    u32 kind;
    sLearns++;
    gSPMatrix(gfx++, &sIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(gfx++, &sIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(gfx++, D_303A758);
    gSPVertex(gfx++, sLearnVtx, 3, 0);
    for (kind = 0; kind < GDX_NRRSHD_KINDS; kind++) {
        if (b->learn[kind]) {
            gfx = NrrShdLearnPoint(gfx, kind);
        }
    }
    gSPMatrix(gfx++, &D_1000000.unk_20208[view], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    return gfx;
}

/* The state the stock section leaves: its setup list (render mode, combiner, geometry mode,
 * texture) and the model-view of its last shadow. Texture and prim are reloaded by every later
 * consumer's own setup list. */
static Gfx* NrrShdWriteTail(Gfx* gfx, const NrrShdBuild* b) {
    if (sTail) {
        gSPDisplayList(gfx++, D_303A758);
    }
    if (b->lastId >= 0) {
        gSPMatrix(gfx++, &D_1000000.unk_20A88[b->lastId], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    }
    return gfx;
}

/* ---- the stub ------------------------------------------------------------------------------- */

static int NrrShdNativeScope(s32 view) {
    if (!gdx3ds_nrr_view_ok(NRR_CAT_SHD, view, gNumPlayers) || gdx3ds_nrr_fx_broken[0]) { /* niview: 2-4P views */
        return 0;
    }
#ifdef EXPANSION_KIT
    if (gGameMode == GAMEMODE_COURSE_EDIT) {
        return gdx3ds_c5tick_testdrive_scope(); /* S5b: the editor's test drive race, nothing else */
    }
#endif
    return gGameMode != GAMEMODE_GP_END_CS && gGameMode != GAMEMODE_COURSE_EDIT;
}

static void* NrrShdStub(void* gfxv, int sec, int view) {
    Gfx* gfx = (Gfx*) gfxv;
    NrrFrame* fr;
    NrrShdBuild b;
    (void) sec;
    if (gRaceIntroTimer >= 280) {
        return gfx; /* the stock section draws nothing; the skip path keeps the speed clamp */
    }
    fr = NrrShdNativeScope(view) ? gdx3ds_nrr_frame_build() : NULL;
    if (fr == NULL) {
        sStockFrames++;
        return NULL;
    }
    memset(&b, 0, sizeof(b));
    b.lastId = -1;
    b.cap = TOTAL_RACER_COUNT + 3;
    if (gNumPlayers > 1) {
        /* niview: the views share the 12 KB frame arena; a multi-view race has only its own racers */
        b.cap = (u32) (sLastRacer - gRacers + 1) + 3;
    }
    b.snap = (GdxNrrShdSnap*) NrrSnapAlloc(fr, sizeof(GdxNrrShdSnap) + sizeof(GdxNrrShdEnt) * (b.cap - 1));
    if (b.snap == NULL) {
        sStockFrames++;
        return NULL; /* the frame arena is full: this frame's shadows are the stock ones */
    }
    b.snap->multi = gNumPlayers > 1 ? 1 : 0;
    NrrShdMtxToFloat(&gGfxPool->unk_20208[view], b.proj);
    NrrShdCollectRacers(&b);
    NrrShdCollectGhosts(&b);
    if (b.needLearn) {
        gfx = NrrShdWriteLearn(gfx, &b, view);
    }
    if (b.snap->n != 0) {
        NrrSnapSet(fr, NRR_CAT_SHD, view, b.snap);
        gfx = (Gfx*) gdx3ds_nrr_write_marker(gfx, NRR_CAT_SHD, view, fr);
    }
    return NrrShdWriteTail(gfx, &b);
}

/* ---- the category init (core contract: called once at boot when nrr_shd is on) ------------- */

/* n60stereo (gdx3ds_stereo_frame.cpp): 1 = the NRR may draw under the [stereo] config. */
int gdx3ds_stereo_nrr_ok(void) __attribute__((weak));

void gdx3ds_nrr_cat_init_shd(void) {
    int taken;
    guMtxIdent(&sIdentityMtx);
    if (&gdx3ds_stereo_nrr_ok != NULL && !gdx3ds_stereo_nrr_ok()) {
        /* [stereo] enabled with [native] stereo_replay=0: the core draws mono only
         * (gdx3ds_nrr_bk_frame_active), a consumed section would vanish from a stereo frame, so the
         * section stays stock (as nrr_mach). With the frame replay (n60stereo) it draws both eyes. */
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[nrr] %s init REFUSED: [stereo] enabled, stereo_replay=0 (mono only); the section stays "
                         "stock\n", "shd");
        }
        return;
    }
    sTail = &gdx3ds_config_get_int != NULL ? gdx3ds_config_get_int("native", "nrr_shd_tail", 0) : 0;
    gdx3ds_nrr_shd_render_init();
    taken = gdx3ds_c5tick_consume(GDX_DLB_SEC_SHD, NrrShdStub);
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[nrr] shd init consume=%d tail=%d (1P race: SHD consumed, learned decal blocks, SpTri's clipper; "
                     "4P / GP ending / editor stock)\n",
                     taken, sTail);
    }
}

/* [nrrfx] receipt counters (game side): learn sequences written, stock-built sections, entries
 * that did not fit the snapshot. */
void gdx3ds_nrr_shd_game_counts(unsigned long out[3]) {
    out[0] = sLearns;
    out[1] = sStockFrames;
    out[2] = sOverflow;
}
