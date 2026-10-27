/* port/3ds/game/gdx3ds_ek_editmesh_game.c -- ekeditmesh `[native] ek_editmesh_track` (o3dsek plan
 * S5c): the game side of the course editor's track-surface preview on the track pipeline. Expansion
 * Kit builds only. (The ground grid and the editor's lines are retained on the render side by
 * `[native] ek_editmesh`, gfx/n64/gdx_ekpreview.cpp; this file is the course surface's half.
 * Receipt run: ek-editline, which builds a 4-point course.)
 *
 * Once the course has chunks the editor draws its surface with func_800A95B4 (course.c): a backward
 * walk of EVERY chunk with the race's own ring emitters (~1.9k triangles for ek-editline's 4-point
 * course), through the interpreter's full CPU transform. The preview is static between edits. With the lever on, decomp-3ds-ek-editmesh.patch
 * makes that walk a [native] track pass (GDXTRK_BEGIN / GDXTRK_END, exactly as Course_Draw's), so its
 * vertex loads are recorded and the render side replays each material run as GPU-transformed draws
 * (track_retained) of the resident ring bake (track_resident) -- index writes only, no per-vertex
 * work. The race bakes at Course_Init; the editor never calls it, so this file re-bakes on the
 * editor's own key:
 *
 *   generation  func_800A1954 (the chunk builder) bumps it on entry: every rebuild of gSegmentChunks
 *               -- an edit, the density switches, a test run's rebuild -- re-keys the bake.
 *   settle      the bake waits until the generation has been unchanged for kSettleWalks preview
 *               walks, so dragging a control point (a rebuild every frame) does not re-bake every
 *               frame; meanwhile the per-frame retained path draws (same pixels, more CPU).
 *
 * Correctness never depends on the key: the render side draws a ring from the bake only when the
 * ring the walk loaded is found there byte for byte, and otherwise takes the per-frame path. A stale
 * or missing bake costs time, not pixels.
 *
 * Lever off (the default): begin returns 0 and no recorder call is made; the list is the stock list
 * (plus course.c's two [c5cat] category words while that clock is armed). Receipts: `[ekmesh] init ...` once, `[ekmesh] walks=...` every 64 preview walks and one
 * `[ekmesh] bake ...` per re-bake (beside the resident bake's own `[trkres] bake` / `adopt`). */
#include "global.h"

#include <stdint.h>

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_native_ready(void) __attribute__((weak));
extern int gdx3ds_native_raw(const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_native_lever(const char* key, int fallback) __attribute__((weak));
extern long long gdx3ds_mprof_now(void) __attribute__((weak));
extern int gdx3ds_c5cat_armed(void) __attribute__((weak));
extern void gdxtrk_course_loaded(void);
extern bool gInCourseEditor;      /* course_gadgets.c */
extern bool gInCourseEditTestRun; /* course_gadgets.c: the test drive draws Course_Draw, not this walk */
extern s32 gSegmentChunkCount;    /* course.c */

#define EKMESH_SETTLE_WALKS 4 /* preview walks with an unchanged generation before a re-bake */
#define EKMESH_WINDOW 64      /* preview walks per [ekmesh] receipt */
#define EKMESH_TICKS_PER_MS 268111.856 /* SYSCLOCK_ARM11 / 1000 */

static int sLever = -1; /* [native] ek_editmesh_track: -1 unlatched, 0 off, 1 on */
static uint32_t sGen = 1;      /* bumped by every chunk rebuild */
static uint32_t sSeenGen = 0;  /* the generation the last walk saw */
static uint32_t sBakedGen = 0; /* the generation the resident bake was taken of (0 = none) */
static uint32_t sSettled = 0;  /* walks since sSeenGen last changed */
static long long sWalkT0 = -1;

static struct {
    uint32_t walks;
    uint32_t bakes;
    uint32_t settling; /* walks drawn while the generation was ahead of the bake */
    uint32_t timed;
    long long walkTicks;
    long long bakeTicks;
} sWin;

static int EditMeshLatch(void) {
    int level;
    int track;
    if (&gdx3ds_native_ready == NULL || &gdx3ds_native_raw == NULL || &gdx3ds_native_lever == NULL ||
        !gdx3ds_native_ready()) {
        return -1; /* the ini is not in yet: latch at a later walk */
    }
    level = gdx3ds_native_raw("ek_editmesh_track", 0) != 0 ? 1 : 0;
    track = gdx3ds_native_lever("track", 0);
    if (level != 0 && track != 1) {
        /* the walk would be recorded into a record nothing replays as a native run */
        if (&gdx_dbg_logf != NULL) {
            gdx_dbg_logf("[ekmesh] ek_editmesh_track=1 IGNORED: needs [native] track=1 (track=%d) -- the stock "
                         "preview walk\n",
                         track);
        }
        level = 0;
    }
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[ekmesh] init ek_editmesh_track=%d track=%d track_retained=%d track_resident=%d settle=%d "
                     "(course editor preview walk -> [native] track pass, resident bake keyed on the chunk "
                     "generation)\n",
                     level, track, gdx3ds_native_raw("track_retained", 0), gdx3ds_native_raw("track_resident", 0),
                     EKMESH_SETTLE_WALKS);
    }
    return level;
}

static int EditMeshClockArmed(void) {
    return &gdx3ds_mprof_now != NULL && &gdx3ds_c5cat_armed != NULL && gdx3ds_c5cat_armed();
}

static double EditMeshMs(long long ticks, uint32_t n) {
    if (n == 0) {
        return 0.0;
    }
    return (double) ticks / EKMESH_TICKS_PER_MS / (double) n;
}

static void EditMeshEmit(void) {
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[ekmesh] walks=%u gen=%u baked=%u bakes=%u settling=%u | walk_ms=%.2f (n=%u) bake_ms=%.2f\n",
                     sWin.walks, sGen, sBakedGen, sWin.bakes, sWin.settling, EditMeshMs(sWin.walkTicks, sWin.timed),
                     sWin.timed, EditMeshMs(sWin.bakeTicks, sWin.bakes));
    }
    sWin.walks = 0;
    sWin.bakes = 0;
    sWin.settling = 0;
    sWin.timed = 0;
    sWin.walkTicks = 0;
    sWin.bakeTicks = 0;
}

/* The resident bake of the chunks as they are now (game thread, before the walk touches anything). */
static void EditMeshBake(void) {
    const int timed = EditMeshClockArmed();
    const long long t0 = timed ? gdx3ds_mprof_now() : 0;
    gdxtrk_course_loaded();
    sBakedGen = sGen;
    sWin.bakes++;
    if (timed) {
        sWin.bakeTicks += gdx3ds_mprof_now() - t0;
    }
    if (&gdx_dbg_logf != NULL) {
        gdx_dbg_logf("[ekmesh] bake gen=%u chunks=%d (after %u settled walks)\n", sGen, (int) gSegmentChunkCount,
                     sSettled);
    }
}

void gdx_ek_editmesh_gen_bump(void) {
    sGen++;
}

/* func_800A95B4's entry, once it has at least 3 chunks: 1 = make this walk a [native] track pass. */
s32 gdx_ek_editmesh_begin(void) {
    if (sLever < 0) {
        sLever = EditMeshLatch();
    }
    if (sLever <= 0 || !gInCourseEditor || gInCourseEditTestRun) {
        return 0;
    }
    if (sGen != sSeenGen) {
        sSeenGen = sGen;
        sSettled = 0;
    } else if (sSettled < 0xFFFFu) {
        sSettled++;
    }
    if (sBakedGen != sGen) {
        if (sSettled >= EKMESH_SETTLE_WALKS) {
            EditMeshBake();
        } else {
            sWin.settling++;
        }
    }
    sWalkT0 = EditMeshClockArmed() ? gdx3ds_mprof_now() : -1;
    return 1;
}

/* After GDXTRK_END: the walk's game-side clock and the window receipt. */
void gdx_ek_editmesh_end(void) {
    if (sWalkT0 >= 0) {
        sWin.walkTicks += gdx3ds_mprof_now() - sWalkT0;
        sWin.timed++;
        sWalkT0 = -1;
    }
    if (++sWin.walks >= EKMESH_WINDOW) {
        EditMeshEmit();
    }
}
