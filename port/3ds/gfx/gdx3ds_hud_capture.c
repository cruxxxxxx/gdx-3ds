/* gdx3ds_hud_capture.c -- [native] hud_batch (C5 Stage 2): the game-side HUD capture.
 *
 * Called by decomp-native-hud-capture.patch right after each captured section returns:
 *   HUD     ([native] hud_batch)    Racer_Draw's marker/countdown tail, Menus_Player1SpecialDraw,
 *                                   Menus_Draw -- markers p=0x5E
 *   shadows ([native] shadow_batch) Racer_Draw's shadow decals -- p=0x5F
 *   fx      ([native] fx_batch)     Racer_Draw's boost flames (before the fx shim bracket) -- 0x5E
 *
 *     gfx = gdxhud_capture_kind(regionStart, gfx, kind);
 *
 * The draw functions themselves are untouched and run in full: their game logic (the minimap's
 * C-left toggle, the check-marker sound, the finish timers, func_i3_801175A4, ...) is never
 * skipped, and they write their display list exactly as they always have. The capture then
 * COMPACTS that region in place (see gdx3ds_hud_record.h):
 *   - "recordable" commands (RDP/RSP state, texture loads, texture/fill rectangles, triangles:
 *     the set in kHudClass) are copied into the per-GfxPool record and each maximal run of them is
 *     replaced by ONE G_NOOP p=0x5E marker;
 *   - RDP sync commands are dropped (their handler is a no-op on this port);
 *   - everything else (G_DL, matrices, vertices -- whose resolution takes the containing list as
 *     a hint -- and other markers) stays in place.
 * The output is never longer than the input (a run of >= 1 word becomes one marker), so the
 * compaction writes at or behind the read cursor and needs no scratch.
 *
 * A region that contains control flow the replay cannot express (G_ENDDL, a branching G_DL,
 * a stray RDPHALF, G_BRANCH_Z, G_LOAD_UCODE, a G_DL into the region itself) or that would
 * overflow the record is left EXACTLY as written and counted as a fallback.
 *
 * [native] nrr_hud_kept (n60hudx, default 0; HUD kind only, needs nrr_hud != 0): a G_DL (push) to a
 * host list that is not scratch memory (gdx3ds_dlw_scratch_ranges: the GfxPools, the frame
 * buffers, the task output) and not in the region -- the HUD's asset setup lists (D_8014940,
 * D_80149D0, D_4007EA8, ...) -- is RECORDED instead of kept, as the first word of a new run (the
 * run before it ends there, as it did at the kept word), and the replay executes the G_DL at the
 * head of that run (the NRR HUD compiles it into the run's program, gdx_nrr_hud.cpp NrrDecDl;
 * every other path walks it in place, HudNestedDl). The list itself stays where it is: only the
 * G_DL word moves, into the run that follows it.
 *
 * A kind is active only when its lever is != 0, the owned interpreter draws ([native] interp=1)
 * and the interp_verify oracle is off (the LUS pass of the oracle cannot replay 0x5E/0x5F).
 * With the lever off gdxhud_armed_kind() is 0 and the patch's call sites do nothing: the
 * display list is byte-identical to the unpatched build.
 *
 * Everything external is weak: this TU also links into the DL-test harness. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gdx3ds_hud_record.h"
#include "gdx3ds_native_cfg.h"
#include "gdx3ds_hotcold.h"
#include "nrr/gdx3ds_nrr.h" /* NRR_CAT_HUD: nrr_hud_kept follows the NRR's own refusal */

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern uint32_t gGameFrameCount __attribute__((weak));
extern int gdx3ds_nrr_level(int cat) __attribute__((weak));
extern void* gGfxPool __attribute__((weak));
extern int gdx3ds_c5tick_hollow_now(void) __attribute__((weak)); /* C5TICK: gdx3ds_c5tick.h */

/* F3DEX2 opcodes (port/3ds/gfx/n64/gdx_gbi.h spells the same values as OP_*). */
enum {
    HUD_OP_NOOP = 0x00,
    HUD_OP_BRANCH_Z = 0x04,
    HUD_OP_TRI1 = 0x05,
    HUD_OP_TRI2 = 0x06,
    HUD_OP_QUAD = 0x07,
    HUD_OP_EXTRAGEOMETRYMODE = 0x3A,
    HUD_OP_TEXTURE = 0xD7,
    HUD_OP_GEOMETRYMODE = 0xD9,
    HUD_OP_LOAD_UCODE = 0xDD,
    HUD_OP_DL = 0xDE,
    HUD_OP_ENDDL = 0xDF,
    HUD_OP_RDPHALF_1 = 0xE1,
    HUD_OP_SETOTHERMODE_L = 0xE2,
    HUD_OP_SETOTHERMODE_H = 0xE3,
    HUD_OP_TEXRECT = 0xE4,
    HUD_OP_TEXRECTFLIP = 0xE5,
    HUD_OP_RDPLOADSYNC = 0xE6,
    HUD_OP_RDPPIPESYNC = 0xE7,
    HUD_OP_RDPTILESYNC = 0xE8,
    HUD_OP_RDPFULLSYNC = 0xE9,
    HUD_OP_SETKEYGB = 0xEA,
    HUD_OP_SETKEYR = 0xEB,
    HUD_OP_SETCONVERT = 0xEC,
    HUD_OP_SETSCISSOR = 0xED,
    HUD_OP_SETPRIMDEPTH = 0xEE,
    HUD_OP_RDPSETOTHERMODE = 0xEF,
    HUD_OP_LOADTLUT = 0xF0,
    HUD_OP_RDPHALF_2 = 0xF1,
    HUD_OP_SETTILESIZE = 0xF2,
    HUD_OP_LOADBLOCK = 0xF3,
    HUD_OP_LOADTILE = 0xF4,
    HUD_OP_SETTILE = 0xF5,
    HUD_OP_FILLRECT = 0xF6,
    HUD_OP_SETFILLCOLOR = 0xF7,
    HUD_OP_SETFOGCOLOR = 0xF8,
    HUD_OP_SETBLENDCOLOR = 0xF9,
    HUD_OP_SETPRIMCOLOR = 0xFA,
    HUD_OP_SETENVCOLOR = 0xFB,
    HUD_OP_SETCOMBINE = 0xFC,
    HUD_OP_SETTIMG = 0xFD,
};
#define HUD_HOST_TAG 0x47445831u /* "GDX1": decomp gbi.h GDX_GFXW1_HOST_TAG */

/* How a word takes part in the capture. */
enum HudClass {
    HUD_KEEP = 0,   /* stays in the main list (a run boundary) */
    HUD_RECORD = 1, /* one recordable word */
    HUD_RECT3 = 2,  /* TEXRECT/TEXRECTFLIP: recordable with its two RDPHALF trailers */
    HUD_DROP = 3,   /* RDP sync: a no-op on this port, dropped */
    HUD_ABORT = 4,  /* control flow the replay cannot express: leave the region stock */
};

enum HudAbort {
    HUD_AB_FLOW = 0, /* ENDDL / BRANCH_Z / LOAD_UCODE / stray RDPHALF */
    HUD_AB_DLBRANCH, /* G_DL without push (a branch) */
    HUD_AB_DLSELF,   /* G_DL into the region itself */
    HUD_AB_RECT,     /* TEXRECT without its trailers */
    HUD_AB_FULL,     /* record capacity */
    HUD_AB_COUNT
};

static const char* const kKindLever[GDXHUD_KIND_COUNT] = { "hud_batch", "shadow_batch", "fx_batch", "ni_menu" };
static const char* const kKindTag[GDXHUD_KIND_COUNT] = { "[hud]", "[shd]", "[fxb]", "[menu]" };

/* Per region kind: -1 unread, 0 off, 1 on, 2 verify. */
static int sMode[GDXHUD_KIND_COUNT] = { -1, -1, -1, -1 };

static GdxHudRecord sRec[2];
static const void* sPoolPtr[2];
static int sPoolNext;

/* Receipt window (64 captured frames), per kind. */
typedef struct {
    uint32_t regions, scanned, recorded, dropped, runs, kept, cuts, keptDl, fits, grouped;
    uint32_t ucodeKept; /* menus-s2b ni_menu_ucode: ucode loads kept in place instead of a cut */
    uint32_t aborts[HUD_AB_COUNT];
    GdxHudReplayStats rp;
} HudWindow;
static HudWindow sWin[GDXHUD_KIND_COUNT];
static uint32_t sWinFrames;

static void HudLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
GDX_COLD static void HudLog(const char* fmt, ...) {
    char buf[480];
    va_list ap;
    if (&gdx_dbg_logf == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gdx_dbg_logf("%s\n", buf);
}

static int HudReadMode(int kind) {
    int mode;
    if (!gdx3ds_native_ready() || &gdx3ds_config_get_int == NULL) {
        return -1;
    }
    mode = gdx3ds_native_raw(kKindLever[kind], 0);
    if (mode < 0 || mode > 2) {
        mode = 0;
    }
    if (mode != 0 && gdx3ds_config_get_int("native", "interp", 0) == 0) {
        HudLog("%s %s=%d needs [native] interp=1 (the owned walker replays the runs): off", kKindTag[kind],
               kKindLever[kind], mode);
        mode = 0;
    } else if (mode != 0 && gdx3ds_native_verify_forces_fold_off()) {
        HudLog("%s %s=%d is off under interp_verify (the LUS pass cannot replay the 0x5E/0x5F markers)",
               kKindTag[kind], kKindLever[kind], mode);
        mode = 0;
    }
    HudLog("%s init %s=%d words=%u runs=%u", kKindTag[kind], kKindLever[kind], mode, (unsigned)GDXHUD_MAX_WORDS,
           (unsigned)GDXHUD_MAX_RUNS);
    return mode;
}

int gdxhud_armed_kind(int kind) {
    if (kind < 0 || kind >= GDXHUD_KIND_COUNT) {
        return 0;
    }
    if (sMode[kind] < 0) {
        sMode[kind] = HudReadMode(kind);
    }
    return sMode[kind] > 0;
}

/* The class of every opcode, as a table: the capture runs over every word of the HUD each
 * frame on the game thread, so it is a byte load, not a switch. */
static const uint8_t kHudClass[256] = {
    [HUD_OP_SETTIMG] = HUD_RECORD,        [HUD_OP_SETTILE] = HUD_RECORD,
    [HUD_OP_SETTILESIZE] = HUD_RECORD,    [HUD_OP_LOADBLOCK] = HUD_RECORD,
    [HUD_OP_LOADTILE] = HUD_RECORD,       [HUD_OP_LOADTLUT] = HUD_RECORD,
    [HUD_OP_FILLRECT] = HUD_RECORD,       [HUD_OP_SETFILLCOLOR] = HUD_RECORD,
    [HUD_OP_SETFOGCOLOR] = HUD_RECORD,    [HUD_OP_SETBLENDCOLOR] = HUD_RECORD,
    [HUD_OP_SETPRIMCOLOR] = HUD_RECORD,   [HUD_OP_SETENVCOLOR] = HUD_RECORD,
    [HUD_OP_SETCOMBINE] = HUD_RECORD,     [HUD_OP_SETOTHERMODE_H] = HUD_RECORD,
    [HUD_OP_SETOTHERMODE_L] = HUD_RECORD, [HUD_OP_RDPSETOTHERMODE] = HUD_RECORD,
    [HUD_OP_SETSCISSOR] = HUD_RECORD,     [HUD_OP_SETPRIMDEPTH] = HUD_RECORD,
    [HUD_OP_SETKEYR] = HUD_RECORD,        [HUD_OP_SETKEYGB] = HUD_RECORD,
    [HUD_OP_SETCONVERT] = HUD_RECORD,     [HUD_OP_GEOMETRYMODE] = HUD_RECORD,
    [HUD_OP_EXTRAGEOMETRYMODE] = HUD_RECORD, [HUD_OP_TEXTURE] = HUD_RECORD,
    [HUD_OP_TRI1] = HUD_RECORD,           [HUD_OP_TRI2] = HUD_RECORD,
    [HUD_OP_QUAD] = HUD_RECORD,
    [HUD_OP_TEXRECT] = HUD_RECT3,         [HUD_OP_TEXRECTFLIP] = HUD_RECT3,
    [HUD_OP_RDPLOADSYNC] = HUD_DROP,      [HUD_OP_RDPPIPESYNC] = HUD_DROP,
    [HUD_OP_RDPTILESYNC] = HUD_DROP,      [HUD_OP_RDPFULLSYNC] = HUD_DROP,
    [HUD_OP_ENDDL] = HUD_ABORT,           [HUD_OP_BRANCH_Z] = HUD_ABORT,
    [HUD_OP_LOAD_UCODE] = HUD_ABORT,      [HUD_OP_RDPHALF_1] = HUD_ABORT,
    [HUD_OP_RDPHALF_2] = HUD_ABORT,
};

static uint8_t HudOp(const GdxHudWord* w) {
    return (uint8_t)(w->w0 >> 24);
}

/* nimenu [native] ni_menu: a MENU region keeps its triangles in place (the walker draws them between
 * the runs, next to the G_VTX / G_MTX they read): the 2D program compiler models no triangle, and a
 * run cut at every G_VTX of a 3D preview would only add markers. */
static uint8_t HudClassOf(const GdxHudWord* w, int kind) {
    const uint8_t op = HudOp(w);
    if (kind == GDXHUD_KIND_MENU && (op == HUD_OP_TRI1 || op == HUD_OP_TRI2 || op == HUD_OP_QUAD)) {
        return HUD_KEEP;
    }
    return kHudClass[op];
}

/* nimenu: a MENU region also keeps a G_SETENVCOLOR that sits between two G_DLs in place -- the
 * machine draws' `G_DL texture-set, G_SETENVCOLOR, G_DL body` triple, which [native] machines_texskip
 * recognises in the main stream (gdx_texskip.cpp TexSkipPeekBody): moved into a run, the pattern breaks
 * and every machine's texture-set list is walked (machine select: +135 loads per frame). */
static uint8_t HudClassIn(const GdxHudWord* in, const GdxHudWord* begin, const GdxHudWord* end, int kind) {
    if (kind == GDXHUD_KIND_MENU && HudOp(in) == HUD_OP_SETENVCOLOR && in > begin && in + 1 < end &&
        HudOp(in - 1) == HUD_OP_DL && HudOp(in + 1) == HUD_OP_DL) {
        return HUD_KEEP;
    }
    return HudClassOf(in, kind);
}

/* The opcode (and region offset) of the last control-flow abort (n60hud3: the receipt says which
 * word kept a region stock). */
static uint8_t sFlowOp;
static uint32_t sFlowAt;
static int sFlowCut = -1; /* [native] hud_flowcut: -1 unread */
/* menus-s2b [native] ni_menu_ucode (default 0; -1 unread): in a MENU region, a ucode load -- the
 * gSPLoadUcode pair, G_RDPHALF_1 (the data segment) then G_LOAD_UCODE -- is a word the walker executes
 * in place, like a G_DL, instead of a flow cut. The records flyby loads one after its table, before
 * the scene's tail and Transition_Draw: everything from it on (the phased-strips wipe on entering the
 * screen, ~560 words a frame for ~60 frames) was walked word by word. The load switches the port's
 * microcode variant (F3DEX2 / F3DLX2_Rej / L3DEX2: one opcode set, gdx_dl.cpp OP_LOAD_UCODE), which
 * only the triangle and lighting paths read: a MENU region keeps its triangles in place, so every word
 * a run records means the same before and after it. Executed in place, it still runs between the
 * same words. */
static int sUcodeKeep = -1;
static int sKept = -1;    /* [native] nrr_hud_kept: -1 unread */

/* n60hudx: the game's scratch ranges (gdx3ds_c5tick_game.c): a G_DL into them names a list built
 * this frame, never recorded. */
typedef struct GdxDlwRange {
    unsigned long lo;
    unsigned long hi;
} GdxDlwRange;
extern int gdx3ds_dlw_scratch_ranges(GdxDlwRange* out, int max) __attribute__((weak));
#define HUD_KEPT_RANGES 8
static GdxDlwRange sKeptRanges[HUD_KEPT_RANGES];
static int sKeptRangeN;


/* A G_DL whose host-tagged target lies inside the region would read words the compaction
 * moves. (A segmented target cannot point into the region: the region is in the GfxPool's
 * master list, which is never a segment base.) */
static int HudDlIntoRegion(const GdxHudWord* w, const GdxHudWord* begin, const GdxHudWord* end) {
    const uintptr_t target = (uintptr_t)w->w1;
    if (w->w1hi != HUD_HOST_TAG) {
        return 0;
    }
    return target >= (uintptr_t)begin && target < (uintptr_t)end;
}

/* Pass 1: can the region be compacted? Returns 0, or -(reason + 1) when it must stay stock.
 * Capacity is checked against upper bounds from the region length (the caller), so this pass
 * only looks for what the replay cannot express. With [native] hud_flowcut (n60hud3, default 1)
 * control flow (G_ENDDL, BRANCH_Z, a ucode load, a stray RDPHALF) no longer keeps the whole region
 * stock: *cut is set to its first such word, the words before it are compacted and everything
 * from it on moves down unchanged (HudCutTail). The post-race records screen loads a ucode after
 * its text: its whole HUD went through the interpreter every frame. What the tail may not hold
 * is a pointer into the region (a G_DL, an RDPHALF operand): moving it would break that. */
static int HudValidate(const GdxHudWord* begin, const GdxHudWord* end, const GdxHudWord** cut, uint32_t* nDl,
                       int kind) {
    const GdxHudWord* w = begin;
    *cut = end;
    *nDl = 0;
    while (w < end) {
        const uint8_t op = HudOp(w);
        const uint8_t cls = kHudClass[op];
        if (kind == GDXHUD_KIND_MENU && sUcodeKeep > 0 && op == HUD_OP_RDPHALF_1 && *cut == end && w + 1 < end &&
            HudOp(w + 1) == HUD_OP_LOAD_UCODE && !HudDlIntoRegion(w, begin, end) &&
            !HudDlIntoRegion(w + 1, begin, end)) {
            sWin[kind].ucodeKept++; /* menus-s2b ni_menu_ucode: both words stay in place (HudCompact) */
            w += 2;
            continue;
        }
        if (cls == HUD_RECT3 && *cut == end) {
            if (w + 2 >= end || HudOp(w + 1) != HUD_OP_RDPHALF_1 || HudOp(w + 2) != HUD_OP_RDPHALF_2) {
                return -HUD_AB_RECT - 1;
            }
            w += 3;
            continue;
        }
        if (cls == HUD_ABORT && *cut == end) {
            sFlowOp = op;
            sFlowAt = (uint32_t)(w - begin);
            if (!sFlowCut) {
                return -HUD_AB_FLOW - 1;
            }
            *cut = w;
        }
        if (op == HUD_OP_DL) {
            (*nDl)++;
        }
        if (op == HUD_OP_DL && *cut == end && ((w->w0 >> 16) & 1u) != 0) {
            return -HUD_AB_DLBRANCH - 1;
        }
        if ((op == HUD_OP_DL || (*cut != end && (op == HUD_OP_RDPHALF_1 || op == HUD_OP_RDPHALF_2))) &&
            HudDlIntoRegion(w, begin, end)) {
            return -HUD_AB_DLSELF - 1;
        }
        w++;
    }
    return 0;
}

static void HudAddReplay(GdxHudReplayStats* dst, const GdxHudReplayStats* src) {
    dst->markers += src->markers;
    dst->words += src->words;
    dst->rects += src->rects;
    dst->loads += src->loads;
    dst->srcHit += src->srcHit;
    dst->srcMiss += src->srcMiss;
    dst->srcInel += src->srcInel;
    dst->hashHit += src->hashHit;
    dst->hashMiss += src->hashMiss;
    dst->verifyOk += src->verifyOk;
    dst->verifyBad += src->verifyBad;
    dst->badRecord += src->badRecord;
    dst->fastTris += src->fastTris;
}

/* One receipt line per armed kind per 64 captured frames, per-frame averages. The replay half
 * is each pool's PREVIOUS frame (the render side fills it after the capture). Columns:
 *   cap   regions captured, words scanned / recorded / sync-dropped / kept in place, runs
 *         (= markers written), aborts by reason (region left stock)
 *   rp    markers replayed, words executed, rects, loads, the source memo (hit/miss/inel), the
 *         content-hash memos, triangles that took the tri memo, verify ok/bad, bad records */
GDX_COLD static void HudEmitKind(int kind, double inv) {
    const HudWindow* w = &sWin[kind];
    HudLog("%s frames=%u mode=%d | cap regions=%.2f scanned=%.1f rec=%.1f drop=%.1f kept=%.1f runs=%.2f "
           "abort flow=%u dlbr=%u dlself=%u rect=%u full=%u | rp markers=%.2f words=%.1f rects=%.1f loads=%.1f "
           "src hit=%u miss=%u inel=%u hash hit=%u miss=%u fasttri=%.1f vfy ok=%u bad=%u badrec=%u flowop=%02x@%u cut=%u keptdl=%.2f fit=%u grouped=%.2f ucode=%.2f",
           kKindTag[kind], sWinFrames, sMode[kind], w->regions * inv, w->scanned * inv, w->recorded * inv,
           w->dropped * inv, w->kept * inv, w->runs * inv, w->aborts[HUD_AB_FLOW], w->aborts[HUD_AB_DLBRANCH],
           w->aborts[HUD_AB_DLSELF], w->aborts[HUD_AB_RECT], w->aborts[HUD_AB_FULL], w->rp.markers * inv,
           w->rp.words * inv, w->rp.rects * inv, w->rp.loads * inv, w->rp.srcHit, w->rp.srcMiss, w->rp.srcInel,
           w->rp.hashHit, w->rp.hashMiss, w->rp.fastTris * inv, w->rp.verifyOk, w->rp.verifyBad, w->rp.badRecord,
           (unsigned)sFlowOp, (unsigned)sFlowAt, w->cuts, w->keptDl * inv, w->fits, w->grouped * inv, w->ucodeKept * inv);
}

static void HudMaybeEmit(void) {
    int k;
    double inv;
    if (sWinFrames < 64) {
        return;
    }
    inv = 1.0 / (double)sWinFrames;
    for (k = 0; k < GDXHUD_KIND_COUNT; k++) {
        if (sMode[k] > 0) {
            HudEmitKind(k, inv);
        }
    }
    memset(sWin, 0, sizeof(sWin));
    sWinFrames = 0;
}

static GdxHudRecord* HudRecordFor(const void* pool, uint32_t frame) {
    int slot = -1;
    int i;
    GdxHudRecord* rec;
    for (i = 0; i < 2; i++) {
        if (sPoolPtr[i] == pool) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = sPoolNext;
        sPoolNext ^= 1;
        sPoolPtr[slot] = pool;
        sRec[slot].magic = 0;
    }
    rec = &sRec[slot];
    if (rec->magic != GDXHUD_MAGIC || rec->frame != frame) {
        if (rec->magic == GDXHUD_MAGIC) {
            int k;
            for (k = 0; k < GDXHUD_KIND_COUNT; k++) {
                HudAddReplay(&sWin[k].rp, &rec->rp[k]); /* that pool's previous frame has been replayed */
            }
        }
        HudMaybeEmit();
        memset(rec->rp, 0, sizeof(rec->rp));
        rec->magic = GDXHUD_MAGIC;
        rec->frame = frame;
        rec->nWords = 0;
        rec->nRuns = 0;
        sWinFrames++;
    }
    return rec;
}

static void HudWriteMarker(GdxHudWord* out, const GdxHudRecord* rec, uint32_t runIndex, int kind) {
    const uint32_t p = kind == GDXHUD_KIND_SHADOW ? GDXHUD_MARKER_P_SHADOW : GDXHUD_MARKER_P;
    out->w0 = ((uint32_t)HUD_OP_NOOP << 24) | (p << 16) | (runIndex & 0xFFFFu);
    out->pad = 0;
    out->w1 = (uint32_t)(uintptr_t)rec;
    out->w1hi = HUD_HOST_TAG;
}

static void HudCloseRun(GdxHudRecord* rec, uint32_t start, GdxHudWord** out, int kind) {
    GdxHudRun* run = &rec->runs[rec->nRuns];
    run->start = (uint16_t)start;
    run->end = (uint16_t)rec->nWords;
    run->kind = (uint8_t)kind;
    run->mode = (uint8_t)sMode[kind];
    run->group = 1;
    run->pad = 0;
    HudWriteMarker(*out, rec, rec->nRuns, kind);
    rec->nRuns++;
    (*out)++;
    sWin[kind].runs++;
}

/* n60results [native] hud_group: a run that follows the previous one with no word in between (a
 * glyph after the glyph before it on a text screen) joins its group instead of writing a marker of
 * its own: the group's one marker replays every run of the group in order (GdxHudRun.group), which
 * is what the walker did marker after marker. groupMax = 0 / 1: a marker per run. */
static void HudCloseRunGrouped(GdxHudRecord* rec, uint32_t start, GdxHudWord** out, int kind, int* group,
                               uint32_t groupMax) {
    GdxHudRun* first;
    if (groupMax <= 1 || *group < 0 || rec->runs[*group].group >= groupMax) {
        *group = (int)rec->nRuns;
        HudCloseRun(rec, start, out, kind);
        return;
    }
    first = &rec->runs[*group];
    rec->runs[rec->nRuns] = *first;
    rec->runs[rec->nRuns].start = (uint16_t)start;
    rec->runs[rec->nRuns].end = (uint16_t)rec->nWords;
    rec->runs[rec->nRuns].group = 0;
    first->group++;
    rec->nRuns++;
    sWin[kind].runs++;
    sWin[kind].grouped++;
}

/* [native] nrr_hud_kept: the lever, read once; on only with the NRR HUD (whose decoder compiles the
 * recorded G_DLs) and the game's scratch ranges known. n3ds: "the NRR HUD" is the NRR core's
 * effective level (gdx3ds_nrr_level), not the raw ini value: with the levers forced on while the NRR
 * refuses (render thread on, interp=0, ...) the capture must keep the stock path. gdx_rt_glue.cpp's
 * NrrHudLatch applies the same rule to its side. */
GDX_COLD static int HudKeptInit(void) {
    int lever = gdx3ds_native_raw("nrr_hud_kept", 0) != 0 ? 1 : 0;
    const int nrrHud = gdx3ds_nrr_level != NULL ? gdx3ds_nrr_level(NRR_CAT_HUD) : 0;
    if (lever && (nrrHud == 0 || gdx3ds_dlw_scratch_ranges == NULL)) {
        lever = 0;
    }
    if (lever) {
        sKeptRangeN = gdx3ds_dlw_scratch_ranges(sKeptRanges, HUD_KEPT_RANGES);
    }
    HudLog("[hud] init nrr_hud_kept=%d (nrr_hud=%d scratch ranges=%d)", lever, nrrHud, sKeptRangeN);
    return lever;
}

/* nimenu: static lists the render side had to WALK inside a run (not pure register words: a colour-image
 * / segment / geometry setup list -- the menus' and the post-race text screens' frame setup) -- such a
 * run never gets a program, so the capture keeps those G_DLs in place instead (the walker enters them
 * between two runs, as without the lever). Filled by the render side only under [native] ni_menu.
 * Written by the render side (gdxhud_kept_refuse), read by the capture; a stale read only picks the
 * other of two exact choices. */
#define HUD_REFUSE_N 64u
static volatile uint32_t sRefuse[HUD_REFUSE_N];

void gdxhud_kept_refuse(uint32_t target) {
    uint32_t h = (target * 0x9E3779B1u) >> 26;
    uint32_t k;
    if (target == 0) {
        return;
    }
    for (k = 0; k < HUD_REFUSE_N; k++) {
        const uint32_t slot = (h + k) & (HUD_REFUSE_N - 1u);
        if (sRefuse[slot] == target) {
            return;
        }
        if (sRefuse[slot] == 0) {
            sRefuse[slot] = target;
            return;
        }
    }
}

static int HudKeptRefused(uint32_t target) {
    uint32_t h = (target * 0x9E3779B1u) >> 26;
    uint32_t k;
    for (k = 0; k < HUD_REFUSE_N; k++) {
        const uint32_t v = sRefuse[(h + k) & (HUD_REFUSE_N - 1u)];
        if (v == target) {
            return 1;
        }
        if (v == 0) {
            return 0;
        }
    }
    return 0;
}

/* A G_DL the HUD capture may record (nrr_hud_kept): a push to a host list outside the region and
 * outside the game's scratch memory (a static asset list, not one built this frame). */
static int HudKeptDl(const GdxHudWord* w, const GdxHudWord* begin, const GdxHudWord* end, int kind) {
    const unsigned long target = (unsigned long)w->w1;
    int k;
    if (((w->w0 >> 16) & 1u) != 0 || w->w1hi != HUD_HOST_TAG || target == 0 || HudDlIntoRegion(w, begin, end)) {
        return 0;
    }
    if (HudKeptRefused((uint32_t)target)) {
        return 0; /* nimenu: filled only under ni_menu (empty otherwise); the race HUD's lists never walk */
    }
    for (k = 0; k < sKeptRangeN; k++) {
        if (target >= sKeptRanges[k].lo && target < sKeptRanges[k].hi) {
            return 0;
        }
    }
    return 1;
}

/* Pass 2: the in-place compaction (the region validated). Returns the new end. `kept`: a G_DL
 * HudKeptDl accepts (n60hudx) is recorded as the FIRST word of a run -- the run before it closes there
 * as it did at the kept G_DL -- so every run keeps its own words (and its program, keyed by them) and
 * gains only the setup list the walker entered just before it. */
static GdxHudWord* HudCompact(GdxHudRecord* rec, GdxHudWord* begin, GdxHudWord* end, int kind, int kept,
                              uint32_t groupMax) {
    GdxHudWord* in = begin;
    GdxHudWord* out = begin;
    GdxHudWord* words = rec->words;
    uint32_t n = rec->nWords;
    uint32_t runStart = 0;
    int runOpen = 0;
    int group = -1; /* hud_group: the open group's first run (-1 = none) */
    while (in < end) {
        const uint8_t cls = HudClassIn(in, begin, end, kind);
        if (kept && HudOp(in) == HUD_OP_DL && HudKeptDl(in, begin, end, kind)) {
            if (runOpen) {
                rec->nWords = n;
                HudCloseRunGrouped(rec, runStart, &out, kind, &group, groupMax);
            }
            runStart = n;
            runOpen = 1;
            words[n++] = *in++;
            sWin[kind].keptDl++;
            continue;
        }
        if (cls == HUD_RECORD) {
            if (!runOpen) {
                runStart = n;
                runOpen = 1;
            }
            words[n++] = *in++;
        } else if (cls == HUD_RECT3) {
            if (!runOpen) {
                runStart = n;
                runOpen = 1;
            }
            words[n++] = *in++;
            words[n++] = *in++;
            words[n++] = *in++;
        } else if (cls == HUD_DROP) {
            in++;
        } else {
            if (runOpen) {
                rec->nWords = n;
                HudCloseRunGrouped(rec, runStart, &out, kind, &group, groupMax);
                runOpen = 0;
            }
            group = -1; /* a word the walker executes between two runs ends the group */
            *out++ = *in++;
        }
    }
    rec->nWords = n;
    if (runOpen) {
        HudCloseRunGrouped(rec, runStart, &out, kind, &group, groupMax);
    }
    return out;
}

/* The words from the region's first control-flow word on, moved down behind the compacted
 * prefix unchanged (the output is never ahead of the input: a forward copy is safe). */
static GdxHudWord* HudCutTail(GdxHudWord* out, GdxHudWord* cut, const GdxHudWord* end) {
    if (out == cut) {
        return (GdxHudWord*)end;
    }
    while (cut < end) {
        *out++ = *cut++;
    }
    return out;
}

/* n60results [native] hud_fit (default 1): the capacity bounds above are pessimistic (every other
 * word a run boundary). The GP standings table's slide-in (~180 glyph runs, ~1,900 words) passed the
 * word bound but not the run bound: the whole region stayed stock (one texture import per glyph,
 * ~40 ms on the Old 3DS). When a bound trips, the validated region's exact words / runs are counted
 * (HudCompact's loop without the writes) and the capture proceeds when they fit. Cold: only a
 * region the bounds refuse reaches it. */
static int sFit = -1; /* -1 unread */

GDX_COLD static int HudFitInit(void) {
    const int lever = gdx3ds_native_raw("hud_fit", 1) != 0 ? 1 : 0;
    HudLog("[hud] init hud_fit=%d (exact capacity count when the bounds refuse a region)", lever);
    return lever;
}

/* n60results [native] hud_group (default 1): runs are grouped (HudCloseRunGrouped) on a text screen --
 * a region with at least HUD_GROUP_MIN_DL recorded setup lists, one per glyph: the GP results and
 * standings tables, name entry, records; the race HUD has 13 and keeps a marker per run. */
#define HUD_GROUP_MIN_DL 32u
#define HUD_GROUP_MAX 255u
static int sGroup = -1; /* -1 unread */

GDX_COLD static int HudGroupInit(void) {
    const int lever = gdx3ds_native_raw("hud_group", 1) != 0 ? 1 : 0;
    HudLog("[hud] init hud_group=%d (text screens: >= %u setup lists per region, <= %u runs per marker)", lever,
           HUD_GROUP_MIN_DL, HUD_GROUP_MAX);
    return lever;
}

static uint32_t HudGroupMax(int kind, uint32_t nDl) {
    if ((kind != GDXHUD_KIND_HUD && kind != GDXHUD_KIND_MENU) || sKept <= 0 || nDl < HUD_GROUP_MIN_DL) {
        return 0;
    }
    if (sGroup < 0) {
        sGroup = HudGroupInit();
    }
    return sGroup ? HUD_GROUP_MAX : 0;
}

/* The words and runs HudCompact(begin, end, kind, kept) would append, without writing (grouping
 * writes no fewer runs, only fewer markers). */
GDX_COLD static void HudCountCompact(const GdxHudWord* begin, const GdxHudWord* end, int kept, int kind,
                                     uint32_t* words, uint32_t* runs) {
    const GdxHudWord* in = begin;
    int runOpen = 0;
    *words = 0;
    *runs = 0;
    while (in < end) {
        const uint8_t cls = HudClassIn(in, begin, end, kind);
        if (kept && HudOp(in) == HUD_OP_DL && HudKeptDl(in, begin, end, kind)) {
            (*runs)++;
            runOpen = 1;
            (*words)++;
            in++;
        } else if (cls == HUD_RECORD || cls == HUD_RECT3) {
            if (!runOpen) {
                (*runs)++;
                runOpen = 1;
            }
            *words += cls == HUD_RECT3 ? 3u : 1u;
            in += cls == HUD_RECT3 ? 3 : 1;
        } else if (cls == HUD_DROP) {
            in++;
        } else {
            runOpen = 0;
            in++;
        }
    }
}

GDX_COLD static int HudFitsExact(const GdxHudRecord* rec, const GdxHudWord* begin, const GdxHudWord* cut, int kept,
                                 int kind, HudWindow* w) {
    uint32_t words;
    uint32_t runs;
    HudCountCompact(begin, cut, kept, kind, &words, &runs);
    if (rec->nWords + words > GDXHUD_MAX_WORDS || rec->nRuns + runs > GDXHUD_MAX_RUNS) {
        return 0;
    }
    w->fits++;
    return 1;
}

/* nrr_hud_kept: which kinds record the G_DLs to static setup lists (the HUD; nimenu: the MENU kind,
 * whose runs the same NRR program compiler takes) */
static int HudKindKeeps(int kind) {
    return (kind == GDXHUD_KIND_HUD || kind == GDXHUD_KIND_MENU) && sKept > 0;
}

void* gdxhud_capture_kind(void* regionStart, void* regionEnd, int kind) {
    GdxHudWord* begin = (GdxHudWord*)regionStart;
    GdxHudWord* end = (GdxHudWord*)regionEnd;
    GdxHudRecord* rec;
    HudWindow* w;
    uint32_t len, words0, runs0;
    GdxHudWord* out;
    const GdxHudWord* cut;
    uint32_t nDl;
    int v;
    int overBound = 0;
    uint32_t grouped0, markers;
    if (!gdxhud_armed_kind(kind) || begin == NULL || end <= begin || &gGameFrameCount == NULL) {
        return regionEnd;
    }
    /* C5TICK [debug] dl_hollow: a hollow tick's list is dropped unwalked -- compacting it is
     * wasted work. Its pool's record is simply not opened for that frame. */
    if (gdx3ds_c5tick_hollow_now != NULL && gdx3ds_c5tick_hollow_now()) {
        return regionEnd;
    }
    rec = HudRecordFor(&gGfxPool != NULL ? gGfxPool : NULL, gGameFrameCount);
    w = &sWin[kind];
    len = (uint32_t)(end - begin);
    w->regions++;
    w->scanned += len;
    /* Upper bounds: every recorded word came from the region, and runs are separated by at
     * least one kept word, so a region of len words yields at most (len + 1) / 2 runs. */
    if (rec->nWords + len > GDXHUD_MAX_WORDS || rec->nRuns + (len + 1u) / 2u > GDXHUD_MAX_RUNS) {
        if (sFit < 0) {
            sFit = HudFitInit();
        }
        if (!sFit) {
            w->aborts[HUD_AB_FULL]++;
            return regionEnd;
        }
        overBound = 1;
    }
    if (sFlowCut < 0) {
        sFlowCut = gdx3ds_native_raw("hud_flowcut", 1) != 0 ? 1 : 0;
        HudLog("[hud] init hud_flowcut=%d", sFlowCut);
    }
    if (sUcodeKeep < 0) {
        sUcodeKeep = gdx3ds_native_raw("ni_menu_ucode", 0) == 1 ? 1 : 0;
        HudLog("[hud] init ni_menu_ucode=%d (a MENU region's ucode load stays in place, not a flow cut)", sUcodeKeep);
    }
    v = HudValidate(begin, end, &cut, &nDl, kind);
    if (v < 0) {
        w->aborts[-v - 1]++;
        return regionEnd;
    }
    if (sKept < 0) {
        sKept = HudKeptInit();
    }
    /* nrr_hud_kept: a recorded G_DL opens a run of its own (one more run than the bound above each) */
    if (HudKindKeeps(kind) && rec->nRuns + (len + 1u) / 2u + nDl > GDXHUD_MAX_RUNS) {
        if (sFit < 0) {
            sFit = HudFitInit();
        }
        overBound = sFit;
        if (!sFit) {
            w->aborts[HUD_AB_FULL]++;
            return regionEnd;
        }
    }
    if (overBound && !HudFitsExact(rec, begin, cut, HudKindKeeps(kind), kind, w)) {
        w->aborts[HUD_AB_FULL]++;
        return regionEnd;
    }
    words0 = rec->nWords;
    runs0 = rec->nRuns;
    grouped0 = w->grouped;
    out = HudCompact(rec, begin, (GdxHudWord*)cut, kind, HudKindKeeps(kind), HudGroupMax(kind, nDl));
    markers = rec->nRuns - runs0 - (w->grouped - grouped0);
    w->recorded += rec->nWords - words0;
    w->kept += (uint32_t)(out - begin) - markers;
    w->dropped += (uint32_t)(cut - begin) - (rec->nWords - words0) - ((uint32_t)(out - begin) - markers);
    if (cut != end) {
        w->cuts++;
        out = HudCutTail(out, (GdxHudWord*)cut, end);
    }
    return out;
}

/* ---- [native] ni_menu: a non-race mode's whole draw, kind MENU (W5, Fable review duplication #5) --
 *
 * This used to be its own file, gdx3ds_menu_capture.c: a thin wrapper around gdxhud_capture_kind
 * above (its only other caller) plus the race/non-race mode gate. Folded in here since the two
 * functions it called are both local to this TU and it was never anything but that call shape.
 *
 * Called by decomp-native-menu-capture.patch around the game-mode draw-table call
 * (func_80069698):
 *
 *     gfx = gdxmenu_capture(start, gfx, mode);
 *
 * For every mode whose draw function is not Race_Draw (title, main menu, machine select /
 * settings, course select, records, options, credits, the GP ending, ...) the region that draw
 * function wrote is compacted exactly as the HUD capture compacts a HUD region (kind MENU above):
 * the RDP-state / texture / rectangle runs move into the per-GfxPool record behind 0x5E markers,
 * and everything else (G_DL to lists built this frame, matrices, vertices, triangles, other
 * markers) stays in place. The render side compiles the runs into NRR programs (gdx_nrr_menu.inc).
 * The race modes keep their own HUD capture brackets (Menus_Draw & co. inside Race_Draw) and are
 * never touched here.
 *
 * Off ([native] ni_menu=0, the default) gdxhud_armed_kind(MENU) is 0 and the list is unchanged. */

/* decomp include/fzx_game.h GameMode (GET_MODE values) */
enum {
    MENU_GM_GP_RACE = 1,
    MENU_GM_PRACTICE = 2,
    MENU_GM_VS_2P = 3,
    MENU_GM_VS_3P = 4,
    MENU_GM_VS_4P = 5,
    MENU_GM_TIME_ATTACK = 14,
    MENU_GM_DEATH_RACE = 21,
};

static int MenuIsRaceDraw(int mode) {
    switch (mode) {
        case MENU_GM_GP_RACE:
        case MENU_GM_PRACTICE:
        case MENU_GM_VS_2P:
        case MENU_GM_VS_3P:
        case MENU_GM_VS_4P:
        case MENU_GM_TIME_ATTACK:
        case MENU_GM_DEATH_RACE:
            return 1;
        default:
            return 0;
    }
}

void* gdxmenu_capture(void* regionStart, void* regionEnd, int mode) {
    if (MenuIsRaceDraw(mode) || !gdxhud_armed_kind(GDXHUD_KIND_MENU)) {
        return regionEnd;
    }
    return gdxhud_capture_kind(regionStart, regionEnd, GDXHUD_KIND_MENU);
}

/* [debug] xition_force=N (1..7, a kTransitionRandomSelection type; 0 = off): every RANDOM
 * transition the game queues becomes type N. The RNG draws are unchanged, so a replay stays in
 * step; only the wipe it shows changes. A test knob so one replay exercises each wipe type. */
static int sXitionForce = -1;
extern int gdx3ds_mp_session_levers_forced(void) __attribute__((weak)); /* gdx3ds_autodrive.c (MP30) */

int gdxmenu_xition_force(void) {
    if ((&gdx3ds_mp_session_levers_forced != 0) && gdx3ds_mp_session_levers_forced()) {
        return 0; /* a lockstep session: the stock wipe everywhere */
    }
    if (sXitionForce < 0) {
        if (!gdx3ds_native_ready() || &gdx3ds_config_get_int == NULL) {
            return 0;
        }
        sXitionForce = gdx3ds_config_get_int("debug", "xition_force", 0);
        if (sXitionForce < 0 || sXitionForce > 7) {
            sXitionForce = 0;
        }
        if (sXitionForce != 0) {
            HudLog("[menu] init xition_force=%d (every random transition is type %d)", sXitionForce, sXitionForce);
        }
    }
    return sXitionForce;
}

/* menus-s4 [native] ni_menu_xition (default 0; only with ni_menu=1; [debug] xition_mask bit 2): called by
 * decomp-native-menu-xition.patch on the frames the mode's draw function did NOT run (func_80069698
 * skips it on the GAMEMODE_CHANGE_INIT frame between a wipe's hide and appear halves): the region is
 * Transition_Draw's words alone (its black fill), compacted as a MENU region exactly as gdxmenu_capture
 * compacts them when the mode's draw ran first. Off, the list is unchanged. -1 unread. */
static int sXitionCapture = -1;

void* gdxmenu_capture_xition(void* regionStart, void* regionEnd, int mode) {
    if (sXitionCapture < 0) {
        if (!gdx3ds_native_ready() || &gdx3ds_config_get_int == NULL) {
            return regionEnd;
        }
        sXitionCapture = (gdx3ds_native_raw("ni_menu_xition", 0) == 1 && gdx3ds_native_raw("ni_menu", 0) == 1 &&
                          (gdx3ds_config_get_int("debug", "xition_mask", 0xFF) & 2) != 0)
                             ? 1
                             : 0;
        HudLog("[menu] init ni_menu_xition=%d (game side: a Transition_Draw without its mode's draw is a MENU region)",
               sXitionCapture);
    }
    if (!sXitionCapture) {
        return regionEnd;
    }
    return gdxmenu_capture(regionStart, regionEnd, mode);
}
