/* gdx3ds_track_sink.c — [native] track: the game-side recorder behind gdx3ds_track_shim.h.
 *
 * Modes ([native] track): 0 = pass-through (every shim macro emits the real GBI; with
 * [native] trkcensus=1 the sink still counts what the native path WOULD record — the M0
 * census), 1 = native (VTX/MOD/template-DL ops are recorded, a G_NOOP marker replaces them at
 * every material-state boundary, and Interpreter::GdxTrackMarker replays the run there),
 * 2 = verify (real GBI AND the record; START/END markers per run let the replay reproduce the
 * run into a native batch, hash it, discard it and compare against the stock draws).
 *
 * Everything external is weak: this TU also links into the DL-test harness. */
#include "gdx3ds_track_record.h"
#include "gdx3ds_track_shim.h"
#include "gdx3ds_track_templates.h"
#include "gdx3ds_native_cfg.h"
#include "nrr/gdx3ds_nrr.h"
#include "nrr/gdx3ds_nrr_trk.h"
#include "nrr/gdx3ds_nrr_crumb.h" /* n60hwdiag */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* n60stereo (gdx3ds_stereo_frame.cpp): 1 = the NRR may draw under the [stereo] config. */
int gdx3ds_stereo_nrr_ok(void) __attribute__((weak));

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gCourseIndex __attribute__((weak)); /* decomp s32: the venue, for receipts only */
extern int gdx3ds_c5cat_armed(void) __attribute__((weak));
extern long long gdx3ds_mprof_now(void) __attribute__((weak));
extern int gNumPlayers __attribute__((weak));                 /* decomp s32 */
/* n60 nrr_trk: the lean walk (game TU) and the bake's index (resident TU); weak for the harness. */
extern int gdxtrk_nrr_game_walk(void* groups, void* groupsEnd, GdxNrrTrkFrame* f, const GdxNrrTrkIndex* ix, int dry,
                                GdxNrrTrkWalkStats* st) __attribute__((weak));
extern uint32_t gdxtrk_nrr_game_verify(const void* rec, uint32_t opFrom, uint32_t opTo, uint32_t* checked,
                                       uint32_t* globalsBad, uint32_t* opsCompared) __attribute__((weak));
extern const GdxNrrTrkIndex* gdxtrk_nrr_index(void) __attribute__((weak));
extern uint32_t gdxtrk_nrr_verify_why_v __attribute__((weak));
extern uint32_t gdxtrk_nrr_verify_join_ops __attribute__((weak)); /* n60trk2: join ops compared */
extern uint32_t gdxtrk_nrr_verify_join_uv __attribute__((weak));  /* join vertices whose stale U/V differed */
extern int gdx3ds_nrr_level(int cat) __attribute__((weak));
extern int gdx3ds_nrr_ni_view(int cat) __attribute__((weak)); /* niview: [native] ni_view */
extern NrrFrame* gdx3ds_nrr_frame_build(void) __attribute__((weak));
extern void* NrrSnapAlloc(NrrFrame* f, uint32_t bytes) __attribute__((weak));
extern void NrrSnapSet(NrrFrame* f, int cat, int view, const void* block) __attribute__((weak));
extern void* gdx3ds_nrr_write_marker(void* gfx, int cat, int view, NrrFrame* f) __attribute__((weak));
#define gdxtrk_nrr_verify_why (&gdxtrk_nrr_verify_why_v)
extern void gdx3ds_c5tick_want_scope(void) __attribute__((weak));
extern int gdx3ds_c5tick_race_scope(void) __attribute__((weak));
extern int gdx3ds_c5tick_scene_scope(void) __attribute__((weak)); /* niview: the records flyby */

/* The asset-bound template arrays (port/gen/AssetBindings.c). Weak: absent in the harness. */
extern unsigned char D_80140F0[] __attribute__((weak));
extern unsigned char D_8014138[] __attribute__((weak));
extern unsigned char D_8014180[] __attribute__((weak));
extern unsigned char D_80141C8[] __attribute__((weak));
extern unsigned char D_8014210[] __attribute__((weak));
extern unsigned char D_8014268[] __attribute__((weak));
extern unsigned char D_80142C0[] __attribute__((weak));
extern unsigned char D_8014308[] __attribute__((weak));
extern unsigned char D_8014350[] __attribute__((weak));
extern unsigned char D_8014398[] __attribute__((weak));
extern unsigned char D_80143E0[] __attribute__((weak));
extern unsigned char D_8014430[] __attribute__((weak));
extern unsigned char D_8014480[] __attribute__((weak));
extern unsigned char D_80144D0[] __attribute__((weak));
extern unsigned char D_8014520[] __attribute__((weak));
extern unsigned char D_8014580[] __attribute__((weak));
extern unsigned char D_80145E0[] __attribute__((weak));
extern unsigned char D_8014640[] __attribute__((weak));

/* The bound arrays above are identity placeholders: the bytes live in the segment-8 RDRAM
 * carve (decomp sys_gfx.c PORT block, warmed at boot by the bridge) at the symbol's segment
 * offset, in N64 word order. Weak: absent in the harness. */
extern unsigned char* gdx_rdram __attribute__((weak));
extern uintptr_t gSegment16C8A0VramStart __attribute__((weak));
static const uint32_t kGdxTrkSegOffset[GDXTRK_TEMPLATE_COUNT] = {
    0x140F0, 0x14138, 0x14180, 0x141C8, 0x14210, 0x14268, 0x142C0, 0x14308, 0x14350,
    0x14398, 0x143E0, 0x14430, 0x14480, 0x144D0, 0x14520, 0x14580, 0x145E0, 0x14640,
};

#define GDXTRK_G_NOOP 0x00u
#define GDXTRK_G_MWO_POINT_ST 0x14

int gdxtrk_active = 0;

static int sMode = -1; /* -1 = config not read yet */
static int sCensus = 0;
static int sRetained = 0; /* [native] track_retained: runs end in p=0x60 markers (C5 Stage 3) */
static int sTemplatesOk = 0;
static int sInitLogged = 0;
static const unsigned char* sTemplatePtr[GDXTRK_TEMPLATE_COUNT];
static GdxTrkRecord sRec[2];
static GdxTrkRecord* sCur = NULL;
static int sPool = 0;
static int sCam = 0;
static uint32_t sRunStart = 0;    /* op index where the current run began */
static int sLastWasVtxRecord = 0; /* the previous shim emission was a recorded VTX op */
static uint32_t sPendingCensus = 0;
static uint32_t sBeginCalls = 0;
static GdxTrkReplayStats sLastRp;
static void TrkReadConfig(void);

/* ---- n60 nrr_trk (gdx3ds_nrr_trk.h): the lean walk's pass state ---- */
static int sNrr = -1;          /* [native] nrr_trk: -1 unread, 0 off, 1 on, 2 verify */
static int sNrrMaxErr = 125;
static uint32_t sNrrJoinsAtBegin = 0; /* sNrrSt.joins at the pass's begin (the join receipt) */
static uint32_t sNrrJoinPasses = 0;
static int sNrrJoins = 0; /* [native] nrr_trk_joins: tunnel-mouth joins on the lean walk (n60trk2) */
static int sNrrPass = 0;       /* this Course_Draw pass belongs to the lean walk */
GdxNrrTrkDrawStats gdx3ds_nrr_trk_stats; /* the render side counts, the receipt below reads */
static GdxNrrTrkFrame sNrrF[2];
/* niview ([native] ni_view bit NRR_CAT_TRK): views 1-3 of a multi-view race, per pool, allocated at
 * the first multi-view pass (view 0 keeps sNrrF: the 1P path is unchanged). */
static GdxNrrTrkFrame* sNrrFv[2][NRR_MAX_VIEWS - 1];
static int sNrrView = 0;  /* the camera of the open lean pass: its snapshot root and marker view */
static int sNrrViews = 1; /* the race's view count (gNumPlayers) at the pass's begin */
static GdxNrrTrkFrame sNrrDry; /* verify: the dry lean walk's ops */
static GdxNrrTrkFrame* sNrrCur = NULL;
static NrrFrame* sNrrCoreFrame = NULL; /* the core's frame of this build: the markers' w1 */
static GdxNrrTrkWalkStats sNrrSt;
static uint32_t sNrrVfyOpFrom = 0;
static uint32_t sNrrPassCount = 0;
static uint32_t sNrrFbLogged = 0;
static struct {
    uint32_t passes, clean, ops, checked, bad, globalsBad, skipped;
} sNrrVfy;

static struct {
    uint32_t vtxOps, verts, dlOps, tris, modOps, runs, realDl, ovrReplace, ovrNoop, fallback;
    uint32_t state[4];
} sC;

/* [trkcat] (c5s3c): the game-side walk, gdxtrk_begin -> gdxtrk_end (the chunk-group walk and
 * its emission into courseVtxBuffer; the cull and grouping before it are not included), and the
 * recorder inside it, timed on every 16th recorder call and scaled by the call count. Armed with
 * the [c5cat] clock and [debug] trkcat=1; static counters only. Read (and reset) by the render
 * side's [trkcat] window (gdx3ds_track_retained.cpp). */
GdxTrkWalkStats gdxtrk_walk_stats;
static int sWalkCfg = -1;
static int sWalkOn = 0;
static long long sWalkT0 = 0;
static long long sDrawT0 = -1; /* Course_Draw entry (gdxtrk_draw_enter), -1 = not timed */
static uint32_t sRecCalls = 0;

static int TrkWalkArmed(void) {
    if (gdx3ds_c5cat_armed == NULL || gdx3ds_mprof_now == NULL || !gdx3ds_c5cat_armed()) {
        return 0;
    }
    if (sWalkCfg < 0 && gdx3ds_config_get_int != NULL) {
        sWalkCfg = gdx3ds_config_get_int("debug", "trkcat", 0) != 0 ? 1 : 0;
    }
    return sWalkCfg > 0;
}

/* Course_Draw's top: the cull + grouping start (they end at gdxtrk_begin). */
void gdxtrk_draw_enter(void) {
    sDrawT0 = TrkWalkArmed() ? gdx3ds_mprof_now() : -1;
}

/* [native] track_resident (gdx3ds_track_resident.cpp): the load-time bake. Weak: the DL-test
 * harness links this TU without it. */
extern void gdx3ds_track_resident_bake(void) __attribute__((weak));

void gdxtrk_course_loaded(void) {
    if (gdx3ds_track_resident_bake != NULL) {
        gdx3ds_track_resident_bake();
    }
}

/* Every 16th recorder call is timed (two clock reads); the rest only counted. */
static long long TrkRecSampleBegin(void) {
    if (!sWalkOn) {
        return -1;
    }
    gdxtrk_walk_stats.recCalls++;
    if ((++sRecCalls & 15u) != 0) {
        return -1;
    }
    return gdx3ds_mprof_now();
}

static void TrkRecSampleEnd(long long t0) {
    if (t0 < 0) {
        return;
    }
    gdxtrk_walk_stats.recSampleTicks += (uint64_t)(gdx3ds_mprof_now() - t0);
    gdxtrk_walk_stats.recSamples++;
}

static void TrkLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void TrkLog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    if (gdx_dbg_logf == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

/* The 18 index lists come from the segment-8 carve (the user's fzerox.o2r, loaded at boot), are
 * decoded once and installed only if they match GDXTRK_TEMPLATES_DIGEST; nothing is compiled in. */
static void TrkInitTemplates(void) {
    unsigned char* const syms[GDXTRK_TEMPLATE_COUNT] = {
        D_80140F0, D_8014138, D_8014180, D_80141C8, D_8014210, D_8014268, D_80142C0, D_8014308, D_8014350,
        D_8014398, D_80143E0, D_8014430, D_8014480, D_80144D0, D_8014520, D_8014580, D_80145E0, D_8014640,
    };
    const uint8_t* src[GDXTRK_TEMPLATE_COUNT];
    int okArr = 0;
    int failedIndex = -1;
    int failedRc = GDXTRK_DECODE_OK;
    int i;
    const unsigned char* seg8 = NULL;
    if (&gdx_rdram != NULL && &gSegment16C8A0VramStart != NULL && gdx_rdram != NULL && gSegment16C8A0VramStart != 0) {
        seg8 = gdx_rdram + gSegment16C8A0VramStart;
    }
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT; i++) {
        sTemplatePtr[i] = syms[i];
        src[i] = seg8 != NULL ? seg8 + kGdxTrkSegOffset[i] : NULL;
    }
    sTemplatesOk = seg8 != NULL && gdxtrk_templates_install(src, &failedIndex, &failedRc);
    if (seg8 != NULL && !sTemplatesOk) {
        if (failedIndex >= 0) {
            const unsigned char* b = src[failedIndex];
            TrkLog("[trk] template %s: carve decode rc=%d (%02X%02X%02X%02X %02X%02X%02X%02X)",
                   kGdxTrkTemplateNames[failedIndex], failedRc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        } else {
            TrkLog("[trk] templates: carve decodes but its digest != 0x%08X (not the retail archive?)",
                   (unsigned)GDXTRK_TEMPLATES_DIGEST);
        }
    }
    /* The bound array is normally an untouched placeholder; count it only as a receipt. */
    for (i = 0; i < GDXTRK_TEMPLATE_COUNT && sTemplatesOk; i++) {
        GdxTrkTemplate t;
        if (syms[i] != NULL && gdxtrk_decode_template(syms[i], kGdxTrkTemplateLen[i], &t) == GDXTRK_DECODE_OK &&
            gdxtrk_template_equal(&t, &gGdxTrkTemplate[i])) {
            okArr++;
        }
    }
    TrkLog("[trk] init mode=%d census=%d templates(carve)=%d/%d templates(arrays)=%d seg8=%p ops=%u runs=%u", sMode,
           sCensus, sTemplatesOk ? GDXTRK_TEMPLATE_COUNT : 0, GDXTRK_TEMPLATE_COUNT, okArr, (const void*)seg8,
           (unsigned)GDXTRK_MAX_OPS, (unsigned)GDXTRK_MAX_RUNS);
}

/* n60 nrr_trk: the NRR core's lever (`[native] nrr=1 nrr_trk=1`; the core refuses it under
 * renderthread, interp=0, interp_verify and replay); `nrr_trk=2` is this category's verify. Needs
 * the legacy recorder (its fallback), the retained replay and the resident bake (its rings). */
static void TrkNrrLatch(void) {
    int resident;
    int raw;
    if (sNrr >= 0) {
        return;
    }
    sNrr = 0;
    if (gdx3ds_config_get_int == NULL || gdx3ds_nrr_level == NULL || !gdx3ds_nrr_level(NRR_CAT_TRK)) {
        return;
    }
    raw = gdx3ds_native_raw("nrr_trk", 0);
    sNrr = raw == 2 ? 2 : 1;
    sNrrMaxErr = gdx3ds_native_raw("nrr_trk_maxerr", 125);
    sNrrJoins = gdx3ds_native_raw("nrr_trk_joins", 0) != 0 ? 1 : 0;
    resident = gdx3ds_native_raw("track_resident", 0);
    if (sMode != 1 || sRetained == 0 || resident == 0) {
        TrkLog("[nrrtrk] [native] nrr_trk=%d needs track=1 track_retained=1 track_resident=1 (track=%d "
               "track_retained=%d track_resident=%d): off",
               sNrr, sMode, sRetained, resident);
        sNrr = 0;
        return;
    }
    if (&gdx3ds_stereo_nrr_ok != NULL && !gdx3ds_stereo_nrr_ok()) {
        TrkLog("[nrrtrk] [native] nrr_trk=%d refused: [stereo] enabled=1 with [native] stereo_replay=0 (the NRR "
               "draws its right eye only through the frame replay): off", sNrr);
        sNrr = 0;
        return;
    }
    if (gdx3ds_c5tick_want_scope != NULL) {
        gdx3ds_c5tick_want_scope();
    }
    TrkLog("[nrrtrk] init nrr_trk=%d (%s) maxerr=%d ops=%u runs=%u frame=%u bytes (the course walk without the "
           "walk: lean walk -> 0x63 runs of resident template ops; 1P race scope; legacy groups on fallback)",
           sNrr, sNrr == 2 ? "verify" : "on", sNrrMaxErr, (unsigned)GDX_NRR_TRK_MAX_OPS,
           (unsigned)GDX_NRR_TRK_MAX_RUNS, (unsigned)sizeof(GdxNrrTrkFrame));
    TrkLog("[nrrtrk] joins nrr_trk_joins=%d (%s)", sNrrJoins,
           sNrrJoins ? "tunnel-mouth joins drawn by the lean walk from resident join vertices"
                     : "tunnel-mouth join groups fall back to the legacy group walk");
}

int gdxtrk_nrr_level(void) {
    if (sMode < 0) {
        TrkReadConfig();
    }
    return sNrr > 0 ? sNrr : 0;
}

int gdxtrk_nrr_joins(void) {
    return gdxtrk_nrr_level() > 0 && sNrrJoins;
}

int gdxtrk_nrr_maxerr(void) {
    return sNrrMaxErr;
}

static void TrkReadConfig(void) {
    if (gdx3ds_config_loaded != NULL && !gdx3ds_config_loaded()) {
        return; /* stay unread; try again next call */
    }
    sMode = 0;
    sCensus = 0;
    if (gdx3ds_config_get_int != NULL) {
        sMode = gdx3ds_native_lever("track", 0); /* [native] all=1 turns it on */
        sCensus = gdx3ds_native_raw("trkcensus", 0) != 0;
    }
    if (sMode < 0 || sMode > 2) {
        sMode = 0;
    }
    TrkInitTemplates();
    if (sMode != 0 && !sTemplatesOk) {
        TrkLog("[trk] templates failed verification: forcing [native] track=0");
        sMode = 0;
    }
    /* [native] track_retained (gdx3ds_track_retained.h): raw, never switched on by `all=1`. The
       record is the same; only the marker differs, so the render side can take the run to the
       GPU or fall back to the CPU replay per run. Needs the native replay (track=1). */
    sRetained = gdx3ds_config_get_int != NULL ? gdx3ds_native_raw("track_retained", 0) : 0;
    if (sRetained < 0 || sRetained > 2) {
        sRetained = 0;
    }
    if (sRetained != 0 && sMode != 1) {
        TrkLog("[trkrr] [native] track_retained=%d needs [native] track=1 (track=%d): off", sRetained, sMode);
        sRetained = 0;
    }
    if (sRetained != 0) {
        TrkLog("[trkrr] sink: runs end in p=0x%02X markers (track_retained=%d)", GDXTRK_MARKER_P_RETAINED, sRetained);
    }
    TrkNrrLatch();
}

static int TrkFirstOpOfRun(void) {
    return sCur->nOps == sRunStart;
}

/* Write the END marker for the pending run into `slot`. */
static void TrkWriteEndMarker(void* slot) {
    const uint32_t run = sCur->nRuns;
    sCur->runs[run].opStart = (uint16_t)sRunStart;
    sCur->runs[run].opEnd = (uint16_t)sCur->nOps;
    sCur->nRuns++;
    const uint32_t p = sRetained != 0 ? GDXTRK_MARKER_P_RETAINED : GDXTRK_MARKER_P_END;
    gdxtrk_gbi_marker(slot, (GDXTRK_G_NOOP << 24) | (p << 16) | (run & 0xFFFFu), sCur);
    sRunStart = sCur->nOps;
    sC.runs++;
}

static void TrkWriteStartMarker(void* slot) {
    const uint32_t run = sCur->nRuns;
    gdxtrk_gbi_marker(slot, (GDXTRK_G_NOOP << 24) | ((uint32_t)GDXTRK_MARKER_P_START << 16) | (run & 0xFFFFu), sCur);
}

/* Room for one more op AND its run's marker. */
static int TrkCanRecord(void) {
    return sCur->nOps < GDXTRK_MAX_OPS && sCur->nRuns < GDXTRK_MAX_RUNS - 1;
}

/* A recording-eligible op could not be recorded: the real command must go out, but only
 * after any pending run has its marker (order). Returns the shim code. */
static int TrkFallbackReal(void* slot, int advanced) {
    sC.fallback++;
    sLastWasVtxRecord = 0;
    if (gdxtrk_pending()) {
        if (!advanced) {
            /* Overwrite site with a pending run: cannot both mark and overwrite; drop the
               overwrite semantics (never observed: the overwrite always follows a VTX). */
            return 0;
        }
        TrkWriteEndMarker(slot);
        return 2;
    }
    return 0;
}

static int TrkAppend(uint8_t kind, uint8_t n, uint8_t slot, uint8_t tmpl, uint32_t arg) {
    GdxTrkOp* op = &sCur->ops[sCur->nOps++];
    op->kind = kind;
    op->n = n;
    op->slot = slot;
    op->tmpl = tmpl;
    op->arg = arg;
    return 1;
}

/* Verify mode: the real command is emitted too. First op of a run takes a START marker in
 * the slot and moves the real command to a fresh slot (2); otherwise the slot is reused (0). */
static int TrkVerifyReturn(void* slot, int firstOfRun, int advanced) {
    if (firstOfRun) {
        TrkWriteStartMarker(slot);
        return 2;
    }
    (void)advanced;
    return 0;
}

/* ---- n60 nrr_trk: the pass, the fallback arming, the verify, the receipt ---------------------- */

/* Is this Course_Draw pass the lean walk's? 1P, the race scope, a baked course, the recorder up. */
static int TrkNrrWhyNot(int cameraIndex) {
    const GdxNrrTrkIndex* ix;
    if (sNrr <= 0 || sMode != 1) {
        return 1;
    }
    if (&gNumPlayers == NULL || gNumPlayers != 1) {
        /* 2-4P (folded viewports): the legacy path unless [native] ni_view takes the views (niview) */
        if (&gNumPlayers == NULL || gNumPlayers < 2 || gNumPlayers > NRR_MAX_VIEWS || cameraIndex < 0 ||
            cameraIndex >= gNumPlayers || gdx3ds_nrr_ni_view == NULL || !gdx3ds_nrr_ni_view(NRR_CAT_TRK)) {
            return 2;
        }
    } else if (cameraIndex != 0) {
        return 1;
    }
    if (gdx3ds_c5tick_race_scope == NULL || !gdx3ds_c5tick_race_scope()) {
        /* menus, the ending: the stock path, exactly; the records flyby with ni_view's records bit
         * (niview: the c5tick scene scope, one view) takes the lean walk */
        if (gdx3ds_c5tick_scene_scope == NULL || !gdx3ds_c5tick_scene_scope() || cameraIndex != 0) {
            return 3;
        }
    }
    if (gdxtrk_nrr_index == NULL || gdxtrk_nrr_game_walk == NULL) {
        return 4;
    }
    ix = gdxtrk_nrr_index();
    return (ix != NULL && ix->nChunks != 0) ? 0 : 5;
}

static int TrkNrrWants(int cameraIndex) {
    const int why = TrkNrrWhyNot(cameraIndex);
    if (why != 0 && sNrr > 0) {
        NRR_CRUMB_FIRST(NRR_CRUMB_TRKREFUSE, 4, "trk pass refused why=%d (1 lever/mode/camera 2 players 3 race scope "
                        "4 unlinked 5 no index) cam=%d", why, cameraIndex);
    }
    return why == 0;
}

/* The core's frame of this build, with the track's root block pointing at our snapshot. */
static int TrkNrrCoreFrame(GdxNrrTrkFrame* f) {
    GdxNrrTrkFrame** root;
    sNrrCoreFrame = gdx3ds_nrr_frame_build != NULL ? gdx3ds_nrr_frame_build() : NULL;
    if (sNrrCoreFrame == NULL || NrrSnapAlloc == NULL || NrrSnapSet == NULL) {
        return 0;
    }
    root = (GdxNrrTrkFrame**)NrrSnapAlloc(sNrrCoreFrame, sizeof(*root));
    if (root == NULL) {
        return 0; /* arena full: the core counts ovf= */
    }
    *root = f;
    NrrSnapSet(sNrrCoreFrame, NRR_CAT_TRK, sNrrView, root);
    return 1;
}

void gdxtrk_nrr_marker(void* slot) {
    if (sNrr == 2 || gdx3ds_nrr_write_marker == NULL) {
        gdxtrk_gbi_noop(slot); /* verify: the dry walk's scratch list */
        return;
    }
    gdx3ds_nrr_write_marker(slot, NRR_CAT_TRK, sNrrView, sNrrCoreFrame);
}

/* The lean pass's snapshot: view 0 the static frame of the pool, views 1-3 allocated once. */
static GdxNrrTrkFrame* TrkNrrViewFrame(int poolIndex, int view) {
    GdxNrrTrkFrame** slot;
    if (view <= 0) {
        return &sNrrF[poolIndex];
    }
    slot = &sNrrFv[poolIndex][view - 1];
    if (*slot == NULL) {
        /* W5 (Fable review, duplication #2): shares the same lazy pool x view -> calloc-on-first-use
         * shape as NrrDressItemsFor (gdx3ds_nrr_dress_game.c) and NrrEfxSpritesFor
         * (gdx3ds_nrr_fx_game.c), which already route through this helper; it logs the allocation
         * itself (byte count + ok/FAILED), so the old pool/view-specific TrkLog line is gone. */
        *slot = (GdxNrrTrkFrame*)gdx3ds_nrr_view_alloc((uint32_t)sizeof(GdxNrrTrkFrame));
    }
    return *slot;
}

static void TrkNrrBegin(int poolIndex, unsigned frame, int cameraIndex) {
    GdxNrrTrkFrame* f;
    sNrrPass = TrkNrrWants(cameraIndex);
    sNrrCur = NULL;
    if (!sNrrPass) {
        return;
    }
    sNrrView = cameraIndex;
    sNrrViews = (&gNumPlayers != NULL && gNumPlayers > 1) ? gNumPlayers : 1;
    f = sNrr == 2 ? &sNrrDry : TrkNrrViewFrame(poolIndex, cameraIndex);
    if (f == NULL) {
        sNrrPass = 0; /* no memory for this view's snapshot: the stock walk */
        return;
    }
    f->magic = GDX_NRR_TRK_MAGIC;
    f->views = (uint32_t)sNrrViews;
    f->frame = frame;
    f->gen = gdxtrk_nrr_index()->gen;
    f->course = &gCourseIndex != NULL ? gCourseIndex : -1;
    f->nOps = 0;
    f->nRuns = 0;
    f->runStart = 0;
    f->fbGroups = 0;
    f->groups = 0;
    sNrrCur = f;
    sNrrJoinsAtBegin = sNrrSt.joins;
    if (sNrr == 1 && !TrkNrrCoreFrame(f)) {
        NRR_CRUMB_FIRST(NRR_CRUMB_TRKREFUSE, 4, "trk pass refused: no core frame (arena full or nrr off)");
        sNrrPass = 0;
        sNrrCur = NULL;
        return;
    }
    if (sNrr == 1) {
        gdxtrk_active = 0; /* the recorder sleeps; a fallback group arms it (gdxtrk_nrr_arm_legacy) */
    } else {
        sNrrVfyOpFrom = sCur->nOps; /* verify: the stock walk records from here */
    }
}

GdxNrrTrkFrame* gdxtrk_nrr_frame(void) {
    return sNrrPass ? sNrrCur : NULL;
}

/* n60hwdiag: the first lean walks of an epoch, bracketed by durable crumbs. */
static int TrkNrrWalkCrumbed(void* groups, void* groupsEnd) {
    int rc;
    gdx3ds_nrr_crumb("trk walk > frame=%p gen=%u course=%d ix=%p", (void*)sNrrCur, (unsigned)sNrrCur->gen,
                     (int)sNrrCur->course, (const void*)gdxtrk_nrr_index());
    rc = gdxtrk_nrr_game_walk(groups, groupsEnd, sNrrCur, gdxtrk_nrr_index(), sNrr == 2, &sNrrSt);
    gdx3ds_nrr_crumb("trk walk < rc=%d ops=%u runs=%u", rc, (unsigned)sNrrCur->nOps, (unsigned)sNrrCur->nRuns);
    return rc;
}

int gdxtrk_nrr_walk(void* groups, void* groupsEnd) {
    if (!sNrrPass || sNrrCur == NULL) {
        if (sNrr > 0) {
            NRR_CRUMB_FIRST(NRR_CRUMB_TRKREFUSE, 4, "trk walk not taken: pass=%d frame=%p begin calls=%lu", sNrrPass,
                            (void*)sNrrCur, (unsigned long)sBeginCalls);
        }
        return 0;
    }
    if (GDX_NRR_CRUMBS_ON() && gdx3ds_nrr_crumb_want(NRR_CRUMB_TRKWALK, 2)) {
        return TrkNrrWalkCrumbed(groups, groupsEnd);
    }
    return gdxtrk_nrr_game_walk(groups, groupsEnd, sNrrCur, gdxtrk_nrr_index(), sNrr == 2, &sNrrSt);
}

void gdxtrk_nrr_arm_legacy(void) {
    if (sNrrPass && sNrr == 1 && sCur != NULL) {
        sRunStart = sCur->nOps;
        sLastWasVtxRecord = 0;
        gdxtrk_active = 1;
    }
}

int gdxtrk_nrr_disarm_legacy(void* slot) {
    int wrote = 0;
    if (!gdxtrk_active) {
        return 0;
    }
    if (gdxtrk_pending()) {
        TrkWriteEndMarker(slot);
        wrote = 1;
    }
    gdxtrk_active = 0;
    return wrote;
}

static void TrkNrrFallbackLog(const GdxNrrTrkFrame* f) {
    if (f->fbGroups == 0) {
        return;
    }
    sNrrFbLogged++;
    if (sNrrFbLogged <= 16u || (sNrrFbLogged & 255u) == 0) {
        TrkLog("[nrr] fallback cat=trk frame=%u groups=%u/%u total=%u (join=%u miss=%u full=%u table=%u layout=%u)",
               f->frame, f->fbGroups, f->groups, sNrrFbLogged, sNrrSt.fbJoin, sNrrSt.fbMiss, sNrrSt.fbFull,
               sNrrSt.fbTable, sNrrSt.fbLayout);
    }
}

static void TrkNrrVerify(void) {
    uint32_t checked = 0;
    uint32_t globalsBad = 0;
    uint32_t opsCompared = 0;
    const uint32_t bad = gdxtrk_nrr_game_verify != NULL ? gdxtrk_nrr_game_verify(sCur, sNrrVfyOpFrom, sCur->nOps,
                                                                                   &checked, &globalsBad, &opsCompared)
                                                        : 0u;
    sNrrVfy.passes++;
    if (sNrrVfy.passes <= 3u) {
        TrkLog("[nrrtrk] verify dbg why=%u from=%u to=%u lean=%u", gdxtrk_nrr_verify_why != NULL ? *gdxtrk_nrr_verify_why : 99u,
               sNrrVfyOpFrom, sCur->nOps, sNrrCur != NULL ? sNrrCur->nOps : 0u);
    }
    if (opsCompared == 0 && checked == 0 && globalsBad == 0 && bad == 0) {
        sNrrVfy.skipped++;
        return;
    }
    sNrrVfy.clean++;
    sNrrVfy.ops += opsCompared;
    sNrrVfy.checked += checked;
    sNrrVfy.bad += bad;
    sNrrVfy.globalsBad += globalsBad;
    if (bad != 0 && sNrrVfy.bad == bad) {
        TrkLog("[nrrtrk] verify MISMATCH frame=%u bad=%u globals=%u ops=%u checked=%u (first)", sNrrCur->frame, bad,
               globalsBad, opsCompared, checked);
    }
}

static void TrkNrrReceipt(void) {
    const GdxNrrTrkDrawStats* d = &gdx3ds_nrr_trk_stats;
    const float n = sNrrSt.passes ? (float)sNrrSt.passes : 1.0f;
    const float rf = d->frames ? (float)d->frames : 1.0f;
    TrkLog("[nrrtrk] frame=%u passes=%u game: groups/f=%.2f ops/f=%.2f runs/f=%.2f loads/f=%.2f tex/f=%.2f fb=%u "
           "(join=%u miss=%u full=%u table=%u layout=%u) | draw: frames=%u runs/f=%.2f ops/f=%.2f tris/f=%.1f "
           "draws/f=%.2f resolves/f=%.2f events/f=%.2f f24over=%u near/f=%.2f maxmpx=%u genmiss=%u bad=%u us/f=%.1f "
           "(resolve=%.1f draw=%.1f) nostate=%u miss gen=%u tex=%u sc=%u own=%u vpsync=%u joins/f=%.2f | merge "
           "held/f=%.2f joined/f=%.2f lost=%u stale=%u drains=%u",
           sNrrCur != NULL ? sNrrCur->frame : 0u, sNrrSt.passes, sNrrSt.groups / n, sNrrSt.ops / n, sNrrSt.runs / n,
           sNrrSt.loads / n, sNrrSt.texSwitch / n, sNrrSt.fbGroups, sNrrSt.fbJoin, sNrrSt.fbMiss, sNrrSt.fbFull,
           sNrrSt.fbTable, sNrrSt.fbLayout, d->frames, d->runs / rf, d->ops / rf, d->tris / rf, d->draws / rf,
           d->resolves / rf, d->events / rf, d->f24over, d->nearRings / rf, d->maxMpx, d->genMiss, d->badRuns,
           (float)((double)d->ticks / 268.111856 / rf), (float)((double)d->resolveTicks / 268.111856 / rf),
           (float)((double)d->drawTicks / 268.111856 / rf), d->noState, d->missGen, d->missTex, d->missScissor,
           d->ownStates, d->vpSyncs, sNrrSt.joins / n, d->held / rf, d->heldJoined / rf, d->heldLost, d->heldStale,
           d->heldDrains);
    if (d->atlasRuns != 0 || d->atlasFail != 0 || d->atlasCopies != 0) {
        TrkLog("[nrrtrk] atlas frame=%u runs/f=%.2f joined/f=%.2f copies=%u fail=%u draws/f=%.2f",
               sNrrCur != NULL ? sNrrCur->frame : 0u, (double)d->atlasRuns / (double)rf, (double)d->atlasJoined / (double)rf,
               d->atlasCopies, d->atlasFail, (double)d->draws / (double)rf);
    }
    if (d->cenFrames != 0) {
        TrkLog("[atlascen] frame=%u course=%d frames=%u runs/f=%.2f same=%u texonly=%u (uv-equal=%u dims-equal=%u) state=%u "
               "diffor=0x%x",
               sNrrCur != NULL ? sNrrCur->frame : 0u, sNrrCur != NULL ? (int)sNrrCur->course : -1, d->cenFrames,
               (double)d->cenRuns / (double)d->cenFrames, d->cenSame, d->cenTexOnly, d->cenTexOnlyUv, d->cenTexOnlyDims,
               d->cenState, d->cenDiffOr);
    }
    if (sNrr == 2) {
        TrkLog("[nrrtrk] verify passes=%u clean=%u skipped=%u ops=%u checked=%u bad=%u globals_bad=%u join_ops=%u "
               "join_uv_stale=%u",
               sNrrVfy.passes, sNrrVfy.clean, sNrrVfy.skipped, sNrrVfy.ops, sNrrVfy.checked, sNrrVfy.bad,
               sNrrVfy.globalsBad, &gdxtrk_nrr_verify_join_ops != NULL ? gdxtrk_nrr_verify_join_ops : 0u,
               &gdxtrk_nrr_verify_join_uv != NULL ? gdxtrk_nrr_verify_join_uv : 0u);
    }
    memset(&sNrrSt, 0, sizeof(sNrrSt));
    memset(&gdx3ds_nrr_trk_stats, 0, sizeof(gdx3ds_nrr_trk_stats));
}

/* n60trk2: the passes that drew a tunnel-mouth join natively (the first 16, then every 256th). */
static void TrkNrrJoinLog(const GdxNrrTrkFrame* f) {
    const uint32_t joins = sNrrSt.joins - sNrrJoinsAtBegin;
    if (joins == 0) {
        return;
    }
    sNrrJoinPasses++;
    if (sNrrJoinPasses <= 16u || (sNrrJoinPasses & 255u) == 0) {
        TrkLog("[nrrtrk] join frame=%u ops=%u passes=%u", f->frame, joins, sNrrJoinPasses);
    }
}

static void TrkNrrEnd(void) {
    sNrrPass = 0;
    if (sNrrCur == NULL) {
        return;
    }
    if (sNrr == 2) {
        TrkNrrVerify();
    } else {
        TrkNrrFallbackLog(sNrrCur);
    }
    TrkNrrJoinLog(sNrrCur);
    if ((++sNrrPassCount & 63u) == 0) {
        TrkNrrReceipt();
    }
}

void gdxtrk_begin(int poolIndex, unsigned frame, int cameraIndex) {
    if (sMode < 0) {
        TrkReadConfig();
    }
    gdxtrk_active = 0;
    sBeginCalls++;
    if (sMode < 0 || (sMode == 0 && !sCensus)) {
        return;
    }
    if (poolIndex < 0 || poolIndex > 1) {
        if (!sInitLogged) {
            sInitLogged = 1;
            TrkLog("[trk] pool index %d out of range: native track disabled for this call", poolIndex);
        }
        return;
    }
    sPool = poolIndex;
    sCam = cameraIndex;
    sCur = &sRec[poolIndex];
    if (sCur->magic != GDXTRK_MAGIC || sCur->frame != frame) {
        sLastRp = sCur->rp; /* the replay of this pool's previous frame is complete by now */
        memset(&sCur->rp, 0, sizeof(sCur->rp));
        sCur->magic = GDXTRK_MAGIC;
        sCur->mode = (uint32_t)sMode;
        sCur->frame = frame;
        sCur->course = &gCourseIndex != NULL ? gCourseIndex : -1;
        sCur->nOps = 0;
        sCur->nRuns = 0;
    }
    sRunStart = sCur->nOps;
    sLastWasVtxRecord = 0;
    sPendingCensus = 0;
    memset(&sC, 0, sizeof(sC));
    gdxtrk_active = 1;
    TrkNrrBegin(poolIndex, frame, cameraIndex);
    sWalkOn = TrkWalkArmed();
    if (sWalkOn) {
        sWalkT0 = gdx3ds_mprof_now();
        if (sDrawT0 >= 0) {
            gdxtrk_walk_stats.cullTicks += (uint64_t)(sWalkT0 - sDrawT0);
        }
    }
    sDrawT0 = -1;
}

void gdxtrk_end(void) {
    if (sNrrPass) {
        TrkNrrEnd();
    }
    if (!gdxtrk_active) {
        return;
    }
    gdxtrk_active = 0;
    if (sWalkOn) {
        gdxtrk_walk_stats.walkTicks += (uint64_t)(gdx3ds_mprof_now() - sWalkT0);
        gdxtrk_walk_stats.passes++;
        gdxtrk_walk_stats.ops += sC.vtxOps + sC.dlOps + sC.modOps;
        gdxtrk_walk_stats.verts += sC.verts;
        sWalkOn = 0;
    }
    /* Verify mode (track=2) always emits the coverage receipt (vok/vbad): a verify run
     * without trkcensus=1 must not be a silent pass. */
    if ((sCensus || sMode == 2) && (sBeginCalls & 63u) == 1u) {
        if (sCensus) {
            TrkLog("[trk] cam=%d mode=%d pool=%d frame=%u vtx=%u verts=%u dl=%u tris=%u mod=%u runs=%u "
                   "state=%u/%u/%u/%u realdl=%u ovr=%u/%u fb=%u ops=%u/%u",
                   sCam, sMode, sPool, sCur->frame, sC.vtxOps, sC.verts, sC.dlOps, sC.tris, sC.modOps, sC.runs,
                   sC.state[0], sC.state[1], sC.state[2], sC.state[3], sC.realDl, sC.ovrReplace, sC.ovrNoop,
                   sC.fallback, sCur->nOps, sCur->nRuns);
        }
        if (sMode != 0) {
            TrkLog("[trk] replay(prev) markers=%u runs=%u vtx=%u verts=%u tris=%u fast=%u exact=%u draws=%u "
                   "comb=%u fb=%u vok=%u vbad=%u gpudraws=%u gputris=%u",
                   sLastRp.markers, sLastRp.runs, sLastRp.vtxOps, sLastRp.verts, sLastRp.tris, sLastRp.fast,
                   sLastRp.exact, sLastRp.draws, sLastRp.combines, sLastRp.fallback, sLastRp.verifyOk,
                   sLastRp.verifyBad, sLastRp.gpuDraws, sLastRp.gpuTris);
        }
    }
}

int gdxtrk_pending(void) {
    return sMode > 0 && sCur != NULL && sCur->nOps > sRunStart;
}

int gdxtrk_marker(void* slot) {
    if (!gdxtrk_pending()) {
        return 0;
    }
    TrkWriteEndMarker(slot);
    sLastWasVtxRecord = 0;
    return 1;
}

int gdxtrk_state(void* slot, int kind) {
    if (kind >= 0 && kind < 4) {
        sC.state[kind]++;
    }
    sLastWasVtxRecord = 0;
    if (sMode == 0) {
        if (sPendingCensus != 0) {
            sC.runs++;
            sPendingCensus = 0;
        }
        return 0;
    }
    return gdxtrk_marker(slot);
}

static int TrkRecordVertex(void* slot, const void* vtx, int n, int v0, int advanced) {
    int firstOfRun;
    sC.vtxOps++;
    sC.verts += (uint32_t)n;
    sPendingCensus++;
    if (sMode == 0) {
        return 0;
    }
    if (n <= 0 || n > 32 || v0 < 0 || v0 + n > 32) {
        return TrkFallbackReal(slot, advanced);
    }
    firstOfRun = TrkFirstOpOfRun();
    if (!advanced) {
        /* Course_SetupChunkPipeTunnelJoinVtx: `gSPVertex(sCourseDisp - 1, …, 5, 16)` overwrites
           the previous slot — the 8-vertex load just recorded (replace the op), or a
           `gSPTexture ON` the stock game thereby DROPS (NOOP the real slot). */
        if (sLastWasVtxRecord && sCur->nOps > sRunStart && sCur->ops[sCur->nOps - 1].kind == GDXTRK_OP_VTX) {
            GdxTrkOp* op = &sCur->ops[sCur->nOps - 1];
            op->n = (uint8_t)n;
            op->slot = (uint8_t)v0;
            op->arg = (uint32_t)(uintptr_t)vtx;
            sC.ovrReplace++;
            return sMode == 2 ? 0 : 1;
        }
        if (!TrkCanRecord()) {
            return TrkFallbackReal(slot, advanced);
        }
        sC.ovrNoop++;
        if (sMode == 2) {
            TrkAppend(GDXTRK_OP_VTX, (uint8_t)n, (uint8_t)v0, 0, (uint32_t)(uintptr_t)vtx);
            sLastWasVtxRecord = 1;
            if (firstOfRun) {
                TrkWriteStartMarker(slot);
            } else {
                gdxtrk_gbi_noop(slot);
            }
            return 2;
        }
        gdxtrk_gbi_noop(slot);
        TrkAppend(GDXTRK_OP_VTX, (uint8_t)n, (uint8_t)v0, 0, (uint32_t)(uintptr_t)vtx);
        sLastWasVtxRecord = 1;
        return 1;
    }
    if (!TrkCanRecord()) {
        return TrkFallbackReal(slot, advanced);
    }
    TrkAppend(GDXTRK_OP_VTX, (uint8_t)n, (uint8_t)v0, 0, (uint32_t)(uintptr_t)vtx);
    sLastWasVtxRecord = 1;
    if (sMode == 2) {
        return TrkVerifyReturn(slot, firstOfRun, advanced);
    }
    return 1;
}

static int TrkRecordDl(void* slot, const void* dl, int advanced) {
    int t;
    int firstOfRun;
    for (t = 0; t < GDXTRK_TEMPLATE_COUNT; t++) {
        if (sTemplatePtr[t] == (const unsigned char*)dl) {
            break;
        }
    }
    if (t == GDXTRK_TEMPLATE_COUNT) {
        /* Setup lists (D_8014008 / D_8014078): real, after any pending run's marker. */
        sC.realDl++;
        sLastWasVtxRecord = 0;
        if (sMode == 0) {
            if (sPendingCensus != 0) {
                sC.runs++;
                sPendingCensus = 0;
            }
            return 0;
        }
        return gdxtrk_marker(slot) ? 2 : 0;
    }
    sC.dlOps++;
    sC.tris += gGdxTrkTemplate[t].ntris;
    sPendingCensus++;
    if (sMode == 0) {
        return 0;
    }
    if (!TrkCanRecord()) {
        return TrkFallbackReal(slot, advanced);
    }
    firstOfRun = TrkFirstOpOfRun();
    TrkAppend(GDXTRK_OP_DL, 0, 0, (uint8_t)t, 0);
    sLastWasVtxRecord = 0;
    if (sMode == 2) {
        return TrkVerifyReturn(slot, firstOfRun, advanced);
    }
    return 1;
}

static int TrkRecordModify(void* slot, int vtx, int where, uint32_t val, int advanced) {
    int firstOfRun;
    sC.modOps++;
    sPendingCensus++;
    if (sMode == 0) {
        return 0;
    }
    if (where != GDXTRK_G_MWO_POINT_ST || vtx < 0 || vtx >= 32 || !TrkCanRecord()) {
        return TrkFallbackReal(slot, advanced);
    }
    firstOfRun = TrkFirstOpOfRun();
    TrkAppend(GDXTRK_OP_MOD, 0, (uint8_t)vtx, 0, val);
    sLastWasVtxRecord = 0;
    if (sMode == 2) {
        return TrkVerifyReturn(slot, firstOfRun, advanced);
    }
    return 1;
}

int gdxtrk_vertex(void* slot, const void* vtx, int n, int v0, int advanced) {
    const long long t0 = TrkRecSampleBegin();
    const int rc = TrkRecordVertex(slot, vtx, n, v0, advanced);
    TrkRecSampleEnd(t0);
    return rc;
}

int gdxtrk_dl(void* slot, const void* dl, int advanced) {
    const long long t0 = TrkRecSampleBegin();
    const int rc = TrkRecordDl(slot, dl, advanced);
    TrkRecSampleEnd(t0);
    return rc;
}

int gdxtrk_modify(void* slot, int vtx, int where, uint32_t val, int advanced) {
    const long long t0 = TrkRecSampleBegin();
    const int rc = TrkRecordModify(slot, vtx, where, val, advanced);
    TrkRecSampleEnd(t0);
    return rc;
}
