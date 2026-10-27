/* port/3ds/gfx/nrr/gdx3ds_nrr_game.c -- NRR core, game half: levers, boot, the per-GfxPool
 * snapshot frames, the 0x63 marker writer and the c5tick consume plumbing. Contract:
 * gdx3ds_nrr.h. Runs on the game fiber (boot: the host loop's first c5tick latch). */
#include "gdx3ds_nrr_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gdx3ds_native_cfg.h"
#include "gdx3ds_nrr_crumb.h" /* n60hwdiag */

extern void gdx_dbg_logf(const char* fmt, ...) __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern int gdx3ds_rt_mode(void) __attribute__((weak));
extern int gdx3ds_tickahead_mode(void) __attribute__((weak)); /* N60TICK4 */
extern int gdx3ds_rt_inited(void) __attribute__((weak));       /* n3ds */
extern uint32_t gGameFrameCount __attribute__((weak));
extern void* gGfxPool __attribute__((weak));
extern int gdx3ds_c5tick_consume(int sec, void* (*stub)(void* gfx, int sec, int view)) __attribute__((weak));
/* niview: this frame's stereo replay (gdx3ds_stereo_frame.cpp); not just the static levers. */
extern int gdx3ds_stereo_nrr_ok(void) __attribute__((weak));

/* Category inits (gdx3ds_nrr.h "how to add a category"): weak, so a category that is not linked
 * yet reads NULL. The core and every category are objects of the executable, never an archive. */
extern void gdx3ds_nrr_cat_init_hud(void) __attribute__((weak));
extern void gdx3ds_nrr_cat_init_trk(void) __attribute__((weak));
extern void gdx3ds_nrr_cat_init_mach(void) __attribute__((weak));
extern void gdx3ds_nrr_cat_init_sky(void) __attribute__((weak));
extern void gdx3ds_nrr_cat_init_dress(void) __attribute__((weak));
extern void gdx3ds_nrr_cat_init_shd(void) __attribute__((weak));
extern void gdx3ds_nrr_cat_init_fx(void) __attribute__((weak));
extern void gdx3ds_nrr_cat_init_selftest(void) __attribute__((weak));
extern void gdx3ds_nrr_cat_init_deco(void) __attribute__((weak));

static const char* const kCatName[NRR_CAT_COUNT] = { "hud", "trk", "mach", "sky", "dress", "shd", "fx", "selftest",
                                                       "deco" };
/* The lever each category reads (design 4.5, milestone 3: n60fx gave shadows and fx their own). */
static const char* const kCatLever[NRR_CAT_COUNT] = {
    "nrr_hud", "nrr_trk", "nrr_mach", "nrr_sky", "nrr_dress", "nrr_shd", "nrr_fx", "nrr_selftest", "nrr_deco",
};

static int sLatched = 0;
static int sBooted = 0;
static int sMaster = 0;
static int sLevel[NRR_CAT_COUNT];
static int sDirect = 0; /* [native] nrr_direct */
static int sNiView = 0; /* [native] ni_view (niview): multi-view categories, bit (1 << cat) */
/* o3ds60 [native] ni_fullview: category bits (ni_view's layout, NRR_NI_FULLVIEW_CATS only) that
 * also apply while Race_Draw builds an MP fullview race (gdx_fv_k() >= 0: the one local view,
 * full screen). Old 3DS preset 0x17A; 0 elsewhere. */
static int sNiFullview = 0;
extern int gdx_fv_k(void) __attribute__((weak)); /* game/gdx3ds_fullview_game.c: Race_Draw's latch */

static NrrFrame sFrames[2];
static int sFrameNext = 0;
static uint32_t sOvf = 0;

/* c5tick section -> consuming category (+1; 0 = none) and whether that section writes the marker. */
static uint8_t sSecCat[GDX_DLB_SEC_COUNT];
static uint8_t sSecMarker[GDX_DLB_SEC_COUNT];
static NrrSnapFn sSnap[NRR_CAT_COUNT];

void gdx3ds_nrr_log(const char* fmt, ...) {
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

const char* gdx3ds_nrr_cat_name(int cat) {
    if (cat < 0 || cat >= NRR_CAT_COUNT) {
        return "?";
    }
    return kCatName[cat];
}

uint32_t gdx3ds_nrr_live_tick(void) {
    return &gGameFrameCount != NULL ? gGameFrameCount : 0u;
}

uint32_t gdx3ds_nrr_take_ovf(void) {
    const uint32_t n = sOvf;
    sOvf = 0;
    return n;
}

/* ------------------------------------------------------------------------ levers ---- */

static const char* RefusalReason(void) {
    if (&gdx3ds_rt_mode != NULL && gdx3ds_rt_mode() != 0) {
        /* N60TICK4: the Old 3DS tick-ahead pipe (render thread on core 0, rt mode 1 sync / 2 pipe)
         * is the one render-thread mode the snapshots are gated for (n60tick4-progress.md).
         * n3ds: otherwise NOT a model gate -- the n60 stack around the NRR is the sequential host
         * loop's (rtMode 0); the New 3DS runs the NRR with the render thread off ([perf] preset =
         * n60 sets renderthread = 0; n3ds-progress.md). */
        const int tickAhead = &gdx3ds_tickahead_mode != NULL && gdx3ds_tickahead_mode() != 0;
        if (!tickAhead || gdx3ds_rt_mode() > 2) {
            return "renderthread (set [debug] renderthread=0; [perf] preset=n60 does)";
        }
    }
    if (gdx3ds_native_raw("interp", 0) == 0) {
        return "needs [native] interp=1"; /* the 0x63 marker is dispatched by the owned walker */
    }
    if (gdx3ds_native_raw("interp_verify", 0) != 0) {
        return "interp_verify"; /* the oracle walks every task twice */
    }
    if (gdx3ds_native_raw("replay", 0) != 0) {
        return "replay"; /* C6 re-submits tagged draws on skipped ticks; NRR draws are untagged (design 6: replay=0) */
    }
    return NULL;
}

static void LatchLevels(void) {
    int cat;
    for (cat = 0; cat < NRR_CAT_COUNT; cat++) {
        sLevel[cat] = sMaster && gdx3ds_native_raw(kCatLever[cat], 0) != 0 ? 1 : 0;
    }
    sNiView = sMaster ? gdx3ds_native_raw("ni_view", 0) : 0;
    sNiFullview = sMaster ? (gdx3ds_native_raw("ni_fullview", 0) & NRR_NI_FULLVIEW_BITS) : 0;
    sDirect = sMaster ? gdx3ds_native_raw("nrr_direct", 0) : 0;
    if (sDirect < 0 || sDirect > 2) {
        sDirect = 0;
    }
}


static void Latch(void) {
    const char* why;
    int want;
    /* N60TICK4: under the tick-ahead pipe the render thread (gdx3ds_nrr_on at task end) and the
     * game fiber (gdx3ds_nrr_boot) can both latch. sLatched is published LAST (release) and read
     * with acquire, so a thread that sees it latched also sees sMaster/sLevel; a concurrent double
     * latch computes the same values from the same ini. */
    if (__atomic_load_n(&sLatched, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (&gdx3ds_config_loaded != NULL && !gdx3ds_config_loaded()) {
        return;
    }
    /* n3ds: the render-thread refusal reads gdx3ds_rt_mode(), which is 0 until gdx3ds_rt_init has
     * run. A latch before that (a boot-time caller) would skip the refusal: defer, every lever reads
     * 0 until then. (sLatched is published at the end, N60TICK4's release store.) */
    if (&gdx3ds_rt_inited != NULL && !gdx3ds_rt_inited()) {
        return;
    }
    want = gdx3ds_native_raw("nrr", 0) != 0;
    if (want) {
        why = RefusalReason();
        if (why != NULL) {
            gdx3ds_nrr_log("[nrr] REFUSED why=%s: every nrr lever reads 0", why);
        } else {
            sMaster = 1;
            LatchLevels();
        }
    }
    __atomic_store_n(&sLatched, 1, __ATOMIC_RELEASE);
}

int gdx3ds_nrr_level(int cat) {
    if (!__atomic_load_n(&sLatched, __ATOMIC_ACQUIRE)) {
        Latch();
    }
    if (cat < 0 || cat >= NRR_CAT_COUNT) {
        return 0;
    }
    return sLevel[cat];
}

/* niview (nimenu W3, review finding M3): this used to also refuse while a stereo frame was active
 * (unless ni_view had NRR_NI_VIEW_STEREO), to dodge a per-eye outer-edge mismatch between the legacy
 * CPU clipper and the NRR's PICA clip during frame replay (race-4p-vs --3d, 29 of 60 SHOTs over the
 * 3D tolerance). Deleted: this function's only callers (gdx3ds_nrr_sky_game.c, gdx3ds_track_sink.c,
 * gdx3ds_nrr_dress_game.c) all gate the call itself on gNumPlayers > 1 first, and
 * Gdx3dsStereo::ReplayEnter (gdx3ds_stereo_frame.cpp) already refuses the frame replay
 * unconditionally whenever gNumPlayers > 1 -- so at every real call site, if this check could ever
 * have fired, frame replay was already refused and the mismatch it guarded against could not occur.
 * Bit 0x200 (NRR_NI_VIEW_STEREO) and its branch were dead. */
/* o3ds60: the ni_view bits in force for this build -- ni_view's, plus ni_fullview's while the
 * race draws MP fullview. Every caller runs inside Race_Draw (the latch's scope), game side. */
static int NiViewBits(void) {
    int bits = sNiView;
    if ((sNiFullview & NRR_NI_FULLVIEW_CATS) != 0 && &gdx_fv_k != NULL && gdx_fv_k() >= 0) {
        bits |= sNiFullview & NRR_NI_FULLVIEW_CATS;
    }
    return bits;
}

int gdx3ds_nrr_ni_view(int cat) {
    if (!sLatched) {
        Latch();
    }
    if (cat < 0 || cat >= NRR_CAT_COUNT || !sLevel[cat]) {
        return 0;
    }
    return (NiViewBits() >> cat) & 1;
}

extern int gNumPlayers __attribute__((weak));

int gdx3ds_nrr_scene_ok(void) {
    if (!sLatched) {
        Latch();
    }
    return sMaster && (sNiView & NRR_NI_VIEW_RECORDS) != 0 && &gNumPlayers != NULL && gNumPlayers == 1;
}

int gdx3ds_nrr_ni_view_race(void) {
    if (!sLatched) {
        Latch();
    }
    return sMaster && (sNiView & NRR_NI_VIEW_RACE) != 0;
}

int gdx3ds_nrr_ni_fullview_race(void) {
    if (!sLatched) {
        Latch();
    }
    return sMaster && (sNiFullview & NRR_NI_VIEW_RACE) != 0;
}

int gdx3ds_nrr_view_ok(int cat, int view, int players) {
    if (players == 1) {
        return view == 0;
    }
    if (players < 2 || players > NRR_MAX_VIEWS || view < 0 || view >= players) {
        return 0;
    }
    return gdx3ds_nrr_ni_view(cat);
}

void* gdx3ds_nrr_view_alloc(uint32_t bytes) {
    void* p = calloc(1, bytes);
    gdx3ds_nrr_log("[nrr] view alloc %lu bytes %s (niview: a category's multi-view buffer)", (unsigned long)bytes,
                   p != NULL ? "ok" : "FAILED");
    return p;
}

int gdx3ds_nrr_on(void) {
    if (!__atomic_load_n(&sLatched, __ATOMIC_ACQUIRE)) {
        Latch();
    }
    return sMaster;
}

int gdx3ds_nrr_direct_level(void) {
    if (!__atomic_load_n(&sLatched, __ATOMIC_ACQUIRE)) {
        Latch();
    }
    return sDirect;
}

static void (*CatInit(int cat))(void) {
    switch (cat) {
        case NRR_CAT_HUD: return gdx3ds_nrr_cat_init_hud;
        case NRR_CAT_TRK: return gdx3ds_nrr_cat_init_trk;
        case NRR_CAT_MACH: return gdx3ds_nrr_cat_init_mach;
        case NRR_CAT_SKY: return gdx3ds_nrr_cat_init_sky;
        case NRR_CAT_DRESS: return gdx3ds_nrr_cat_init_dress;
        case NRR_CAT_SHD: return gdx3ds_nrr_cat_init_shd;
        case NRR_CAT_FX: return gdx3ds_nrr_cat_init_fx;
        case NRR_CAT_SELFTEST: return gdx3ds_nrr_cat_init_selftest;
        case NRR_CAT_DECO: return gdx3ds_nrr_cat_init_deco;
        default: return NULL;
    }
}

static void InitCategories(void) {
    int cat;
    for (cat = 0; cat < NRR_CAT_COUNT; cat++) {
        void (*init)(void) = CatInit(cat);
        if (!sLevel[cat]) {
            continue;
        }
        if (init == NULL) {
            gdx3ds_nrr_log("[nrr] cat=%s lever on but the category is not linked: off", kCatName[cat]);
            sLevel[cat] = 0;
            continue;
        }
        if (GDX_NRR_CRUMBS_ON()) {
            gdx3ds_nrr_crumb("cat init > %s", kCatName[cat]);
        }
        init();
        if (GDX_NRR_CRUMBS_ON()) {
            gdx3ds_nrr_crumb("cat init < %s level=%d", kCatName[cat], sLevel[cat]);
        }
    }
}

void gdx3ds_nrr_boot(void) {
    if (sBooted) {
        return;
    }
    Latch();
    if (!__atomic_load_n(&sLatched, __ATOMIC_ACQUIRE)) {
        return; /* ini not parsed yet: the next latch retries */
    }
    sBooted = 1;
    if (!sMaster) {
        return;
    }
    InitCategories();
    gdx3ds_nrr_log("[nrr] init nrr=1 hud=%d trk=%d mach=%d sky=%d dress=%d shd=%d fx=%d selftest=%d deco=%d direct=%d (%s) ni_view=0x%x ni_fullview=0x%x | "
                   "arena=%u x2 states=%d marker=0x%02x (hot bytes: tools/native/nrr_size.sh)",
                   sLevel[NRR_CAT_HUD], sLevel[NRR_CAT_TRK], sLevel[NRR_CAT_MACH], sLevel[NRR_CAT_SKY],
                   sLevel[NRR_CAT_DRESS], sLevel[NRR_CAT_SHD], sLevel[NRR_CAT_FX], sLevel[NRR_CAT_SELFTEST],
                   sLevel[NRR_CAT_DECO], sDirect, sDirect == 1 ? "direct words" : (sDirect == 2 ? "verify: citro3d words kept" : "citro3d"), (unsigned)sNiView,
                   (unsigned)sNiFullview, (unsigned)NRR_FRAME_ARENA_BYTES, NRR_STATE_MAX, (unsigned)GDX3DS_NRR_MARKER_P);
}

/* ------------------------------------------------------------------ snapshot frames ---- */

static void ResetFrame(NrrFrame* f, const void* pool, uint32_t tick) {
    f->magic = NRR_FRAME_MAGIC;
    f->tick = tick;
    f->pool = pool;
    f->used = 0;
    f->ovf = 0;
    memset(f->root, 0, sizeof(f->root));
}

static NrrFrame* FrameSlotFor(const void* pool) {
    int i;
    for (i = 0; i < 2; i++) {
        if (sFrames[i].magic == NRR_FRAME_MAGIC && sFrames[i].pool == pool) {
            return &sFrames[i];
        }
    }
    i = sFrameNext;
    sFrameNext ^= 1;
    sFrames[i].magic = 0;
    return &sFrames[i];
}

static unsigned sStereoRefused; /* niview: gdx3ds_nrr_frame_build refusals, this frame's replay unavailable */
/* niview: diagnostic for the coordinator's "build vs replay disagreement" question (2026-10-15) --
 * edge-triggered (at most one line per transition, not per category-call, dedup'd by tick), so this
 * is cheap enough to leave on permanently: real refusals (HoldFrame/QueueRoom) are rare in production
 * (the test-only `[debug] stereo_force_refuse_n` lever that used to force them for gating is gone --
 * nimenu W1, docs/research/native-review-2026-10-15.md). */
static uint32_t sEdgeTick = 0xFFFFFFFFu;
static int sEdgeRefused = 0;

/* niview: every category's stub calls gdx3ds_nrr_frame_build() right after its own admission check,
 * 1P and multi-view alike (sky/trk/shd/fx/dress/mach all call it) -- a NULL return is already the
 * category's "fall back to the stock interpreter" signal (Background_Draw and friends), so this is
 * the one shared, already-hot place to close the stereo gap for every category at once, mach
 * included, instead of touching each category's own admission function. */
NrrFrame* gdx3ds_nrr_frame_build(void) {
    const void* pool;
    uint32_t tick;
    NrrFrame* f;
    if (!sMaster) {
        return NULL;
    }
    tick = gdx3ds_nrr_live_tick();
    if (&gdx3ds_stereo_nrr_ok != NULL && !gdx3ds_stereo_nrr_ok()) {
        sStereoRefused++;
        if ((sStereoRefused & 255u) == 0u) {
            gdx3ds_nrr_log("[nrr] stereo refused frame_build=%u (this frame's replay unavailable: "
                           "every category falls back to stock)",
                           sStereoRefused);
        }
        if (tick != sEdgeTick && sEdgeRefused == 0) {
            gdx3ds_nrr_log("[nrr] stereo refused EDGE tick=%lu (ok->refuse transition)", (unsigned long) tick);
            sEdgeRefused = 1;
        }
        sEdgeTick = tick;
        return NULL;
    }
    if (tick != sEdgeTick && sEdgeRefused != 0) {
        gdx3ds_nrr_log("[nrr] stereo refused EDGE tick=%lu (refuse->ok transition)", (unsigned long) tick);
        sEdgeRefused = 0;
    }
    sEdgeTick = tick;
    pool = &gGfxPool != NULL ? gGfxPool : NULL;
    tick = gdx3ds_nrr_live_tick();
    f = FrameSlotFor(pool);
    if (f->magic != NRR_FRAME_MAGIC || f->tick != tick || f->pool != pool) {
        ResetFrame(f, pool, tick);
    }
    return f;
}

void* NrrSnapAlloc(NrrFrame* f, uint32_t bytes) {
    const uint32_t need = (bytes + 7u) & ~7u;
    uint8_t* p;
    if (f == NULL || need > NRR_FRAME_ARENA_BYTES - f->used) {
        if (f != NULL) {
            f->ovf++;
        }
        sOvf++;
        return NULL;
    }
    p = (uint8_t*)f->arena + f->used;
    f->used += need;
    memset(p, 0, need);
    return p;
}

void NrrSnapSet(NrrFrame* f, int cat, int view, const void* block) {
    if (f == NULL || cat < 0 || cat >= NRR_CAT_COUNT || view < 0 || view >= NRR_MAX_VIEWS) {
        return;
    }
    f->root[cat][view] = block;
}

const void* NrrSnapGet(const NrrFrame* f, int cat, int view) {
    if (f == NULL || cat < 0 || cat >= NRR_CAT_COUNT || view < 0 || view >= NRR_MAX_VIEWS) {
        return NULL;
    }
    return f->root[cat][view];
}

uint32_t NrrFrameTick(const NrrFrame* f) {
    return f != NULL ? f->tick : 0u;
}

/* The port's Gfx: w0, pad, then the 64-bit w1 whose high half tags a host pointer
 * (gdx3ds_hud_record.h GdxHudWord). */
typedef struct NrrGfxWord {
    uint32_t w0;
    uint32_t pad;
    uint32_t w1;
    uint32_t w1hi;
} NrrGfxWord;

void* gdx3ds_nrr_write_marker(void* gfx, int cat, int view, NrrFrame* f) {
    NrrGfxWord* w = (NrrGfxWord*)gfx;
    if (w == NULL) {
        return gfx;
    }
    w->w0 = (GDX3DS_NRR_MARKER_P << 16) | ((uint32_t)(view & 0xFF) << 8) | (uint32_t)(cat & 0xFF); /* G_NOOP = 0 */
    w->pad = 0;
    w->w1 = (uint32_t)(uintptr_t)f;
    w->w1hi = NRR_HOST_TAG;
    NRR_CRUMB_FIRST(NRR_CRUMB_SNAP + (cat & (NRR_CRUMB_CATS - 1)), 2, "snapshot cat=%s view=%d frame=%p used=%lu ovf=%lu (marker written)",
                    gdx3ds_nrr_cat_name(cat), view, (void*)f, f != NULL ? (unsigned long)f->used : 0ul,
                    f != NULL ? (unsigned long)f->ovf : 0ul);
    return w + 1;
}

/* ------------------------------------------------------------------- consume path ---- */

static void* ConsumeStub(void* gfx, int sec, int view) {
    int cat;
    NrrFrame* f;
    if (sec < 0 || sec >= GDX_DLB_SEC_COUNT || sSecCat[sec] == 0 || !sSecMarker[sec]) {
        return gfx;
    }
    cat = sSecCat[sec] - 1;
    f = gdx3ds_nrr_frame_build();
    if (f == NULL || sSnap[cat] == NULL || !sSnap[cat](f, cat, view)) {
        return gfx;
    }
    return gdx3ds_nrr_write_marker(gfx, cat, view, f);
}

int gdx3ds_nrr_register(int cat, uint32_t secMask, NrrSnapFn snap) {
    int sec;
    int taken = 0;
    int markerSet = 0;
    if (cat < 0 || cat >= NRR_CAT_COUNT || !gdx3ds_nrr_level(cat) || &gdx3ds_c5tick_consume == NULL) {
        return 0;
    }
    sSnap[cat] = snap;
    for (sec = 0; sec < GDX_DLB_SEC_COUNT; sec++) {
        if ((secMask & (1u << sec)) == 0) {
            continue;
        }
        if (!gdx3ds_c5tick_consume(sec, ConsumeStub)) {
            continue; /* c5tick printed the refusal */
        }
        sSecCat[sec] = (uint8_t)(cat + 1);
        sSecMarker[sec] = markerSet ? 0 : 1;
        markerSet = 1;
        taken++;
    }
    gdx3ds_nrr_log("[nrr] register cat=%s secMask=0x%x taken=%d", kCatName[cat], (unsigned)secMask, taken);
    if (GDX_NRR_CRUMBS_ON()) {
        gdx3ds_nrr_crumb("register cat=%s secMask=0x%x taken=%d", kCatName[cat], (unsigned)secMask, taken);
    }
    return taken;
}
