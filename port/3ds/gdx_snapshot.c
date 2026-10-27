/* port/3ds/gdx_snapshot.c -- mp-snapshot: game-state SNAPSHOT / RESTORE, the synctest gate and the
 * hardware cost probe. Portable contract: port/gdx_snapshot.h. Background: the rollback study,
 * artifacts/mp-netcode-research/rollback.md (§1 state, §2.4 side effects, §3.4 synctest).
 *
 * TICK BOUNDARY. gdx3ds_snapshot_frame_end runs at the END of a host iteration: the game fiber is
 * parked at the VI wait (Game_ThreadEntry -> func_80067D64/func_80067E98 -> osRecvMesg), so every
 * fiber frame is the same and holds no live local; restoring the image there is sound and no
 * fiber stack is saved. The render thread is joined (gdx3ds_rt_join_idle) before any restore.
 *
 * LAYOUT (port/3ds/gdx3ds_snapshot_{data,bss}.ld, via gdx3ds_snapshot_ld.cmake). Two linker-grouped blocks -- the decomp .data and the
 * decomp .bss/COMMON, each with the port's GDX_SNAP_STATE_* statics appended -- plus the dynamic
 * slots (the CPU-AI arena buffer) and any REQUIRED symbol the link left outside (the game TU's
 * table, port/3ds/game/gdx3ds_snapshot_game.c; covered as an extra region, with a receipt).
 *
 * A WINDOW (both modes) is: save S at tick T -> the next N ticks run for real (rendered, audible),
 * their final pads and their per-tick outside inputs recorded -> at T+N: restore S and re-run the
 * same N ticks SILENTLY (no render: the GFX task is acked undrawn; no audio: every AudioThread
 * command is dropped and the one-shot triggers are gated) -> the state must equal the real T+N.
 * The real run goes first, so a window crossed by a mode change is simply abandoned unrestored.
 *
 *   [debug] snap_test = 1   THE GATE (emulator): windows at pseudo-random ticks (snap_seed,
 *                           snap_gap_min..snap_gap_max frames apart, N = 1..snap_n_max), the real
 *                           T+N image kept in a second buffer and compared BYTE FOR BYTE with the
 *                           resimulated one. A difference is a MISS: receipts name each differing
 *                           run (address, region, offset); the image is then healed from the real
 *                           copy (snap_heal, default 1) so one miss does not cascade. Also watches
 *                           the nosnap block (it must not change in a race) and, with snap_wide = K,
 *                           block-hashes ALL static memory + the RDRAM arena for the first K windows
 *                           to find state the snapshot does not hold (receipt: [snap] WIDE).
 *   [debug] snap_probe = 1  THE HARDWARE PROBE (single player only): every snap_probe_every frames
 *                           (default 180) one window of snap_probe_ticks (default 4) with one buffer:
 *                           the real T+N image is HASHED, restored, resimulated and hashed again.
 *                           Receipt: bytes, save_us, restore_us, resim_us (+ per tick), hash_us,
 *                           MB/s, ok, and the audio gate's counts (commands dropped, one-shots
 *                           de-duplicated / new).
 *   [debug] snap_census = 1 THE DIRTY CENSUS (emulator, snap-fast): one bit per 32-byte block of the
 *                           static image, set when the block changes between two in-race frames.
 *                           Receipts `[snap] census` / `cregion` / `cmap`; tools/dev/snapcensus.py maps
 *                           the bitmap onto the ELF's symbols (the NOSNAP hole candidates).
 * All default 0: nothing is allocated and every hook is one int test.
 *
 * SNAP-FAST (the cost levers; each -1 = cycled per window, so one probe run A/Bs every variant on
 * the console and the gate proves every path byte-exact; >= 0 pins it):
 *   [debug] snap_copy = 0 newlib memcpy (card-4) | 1 LDM/STM 32-byte bursts | 2 the same + PLD
 *                       (port/3ds/gdx3ds_snapcopy.c; newlib's memcpy is the generic C word loop)
 *   [debug] snap_sync = 0 full restore | 1 restore only the 256-byte blocks that differ from the
 *                       saved image (a race changes ~22 KB of the image per tick)
 *   [debug] snap_buf  = 0 linearAlloc (card-4) | 1 heap (memalign); the probe cycles both
 *   [debug] snap_holes = 1 (default) carve the runtime NOSNAP table out of the image
 *                       (gdx3ds_snapshot_game.c: objects no race writes -- the course editor, the
 *                       64DD file system, menu textures; chosen by the census, watched by the gate);
 *                       0 = card-4's full image.
 * The probe also times a RESAVE (`resave_us`): a sync save of the real T+N over the T image, i.e.
 * run-ahead's next per-frame save.
 *
 * Receipts: `[snap] init ...` (layout + bytes) once at arm, `[snap] test #k ...` / `[snap] probe
 * #k ...` per window, `[snap] MISS ...` + `[snap]   diff ...`, `[snap] skip ...`, `[snap] NOSNAP
 * changed ...`, `[snap] WIDE ...`, `[snap] REQUIRED ... outside`.
 *
 * STATE OUTSIDE THE DECOMP BLOCKS that the simulation reads, and how each is handled:
 *   - port statics moved INTO the snapshot (GDX_SNAP_STATE_BSS / _DATA): the input bridge's edge
 *     accumulators, presence latch, baseline flag and input tick (port/input_bridge.c); the VI's
 *     current / next framebuffer (port/n64_vi.c); gSegments (port/decomp_port.c); the RDRAM
 *     arena's FRONT/BACK bump cursor (port/decomp_port.c); the EK countdown hold (autodrive.c).
 *   - covered as an extra region: gArenaStartPtrs (defined by the generated port/gen/LinkStubs.c).
 *   - dynamic region: the CPU-AI buffer in the RDRAM arena (cpu.c, slot 0).
 *   - per-tick INPUTS (gdx_snap_tick_input, recorded by the real tick, replayed by a resim): the EK
 *     hold's disk-BGM load and every pad's BGM-ready bit; the C5TICK build-drop prediction.
 *
 * MISSES THE GATE FOUND (and the fix; the patch README carries the decomp half):
 *   1. "NOSNAP changed" every window: gSegmentChunks is rewritten by Course_Draw's chunk cull
 *      (depth / drawState) every frame. Draw-only and recomputed before it is read: moved to the
 *      unwatched GDX_NOSNAP_DRAW class.
 *   2. MISS in the heavy stretches of race-gp-cup (O3DS lane, 13 of 257 windows): Racer
 *      shadowMatrixInit, course.c's chunk groups, display-list cursors, the texture registry. The
 *      C5TICK hollow build (dl_hollow, n60 preset) skips pure draw sections on a build the
 *      frameskip governor predicts dropped; the prediction reads the governor's clock, so a resim
 *      hollowed different builds. Fix: the prediction is a tick input (GDX_SNAP_IN_DL_BUILD_DROP).
 *   3. MISS, the texture registry D_800E33E0 only (7 windows): glyphs registered lazily during a
 *      race carve arena memory, and the port's bump cursor (gdx_rdram_bump) was not restored, so a
 *      resim registered them at new addresses. Fix: the cursor is snapshot state.
 * The gameplay digest ([gstate]) matched in every MISS window: all three were draw-side or
 * allocator state, and with snap_heal the replay stayed identical to an un-snapshotted run.
 *
 * NOTE FOR ROLLBACK: a resim must replay the real ticks' build-drop decisions only to be byte-exact
 * with them; a production rollback can hollow every resim build instead (they are never drawn). */
#include "gdx_snapshot.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "gdx3ds_config.h"
#include "gdx3ds_filelog.h"
#include "gdx3ds_renderthread.h"
#include "gdx3ds_snapcopy.h"

/* ---- links into the rest of the port ------------------------------------------------------------ */

extern unsigned long long gdx_input_tick(void);       /* port/input_bridge.c */
extern int gdx_controller_poll_resim(void);           /* port/input_bridge.c */
extern void gdx_vi_tick(void);                        /* port/n64_vi.c */
extern void gdx_sched_drain_deferred_wakes(void);     /* port/n64_sched.c */
extern void gdx_dispatch(void);                       /* port/n64_sched.c */
extern int gdx_sched_on_host_thread(void);            /* port/n64_sched.c */
extern unsigned int gdx_gstate_hash(int* outRacers);  /* port/decomp_port.c */
extern void gdx_rdram_probe_stats(size_t* out);       /* port/decomp_port.c */
extern unsigned char* gdx_rdram;                      /* port/decomp_port.c */
extern int gdx3ds_mp_active(void) __attribute__((weak)); /* port/3ds/net: a lockstep session */

typedef struct GdxSnapRequired {
    const void* ptr;
    unsigned int size;
    const char* name;
} GdxSnapRequired;
extern int gdx3ds_snap_game_scope(void);              /* port/3ds/game/gdx3ds_snapshot_game.c */
extern int gdx3ds_snap_game_mode_word(void);
extern int gdx3ds_snap_game_single_player(void);
extern int gdx3ds_snap_game_required(const GdxSnapRequired** out);
extern int gdx3ds_snap_game_nosnap(const GdxSnapRequired** out);

/* The blocks (port/3ds/gdx3ds_snapshot_{data,bss}.ld). Weak: without that script they resolve to 0 and the module
 * refuses to arm rather than snapshot nothing. */
extern unsigned char __gdx_snap_data_start[] __attribute__((weak));
extern unsigned char __gdx_snap_data_end[] __attribute__((weak));
extern unsigned char __gdx_snap_bss_start[] __attribute__((weak));
extern unsigned char __gdx_snap_bss_end[] __attribute__((weak));
extern unsigned char __gdx_nosnap_bss_start[] __attribute__((weak));
extern unsigned char __gdx_nosnap_bss_end[] __attribute__((weak));
extern unsigned char __end__[]; /* 3dsx.ld: the end of the static image */

/* ---- constants ------------------------------------------------------------------------------------ */

enum { SNAP_OFF = 0, SNAP_TEST = 1, SNAP_PROBE = 2, SNAP_CENSUS = 3 };

/* The buffer kinds a variant can copy into (snap-fast). */
enum { SNAP_BUF_LINEAR = 0, SNAP_BUF_HEAP = 1, SNAP_BUF_KINDS = 2 };

#define SNAP_MAX_REGIONS 160
#define SNAP_MAX_HOLES 96
#define SNAP_ALIGN 32u               /* every region starts on a cache line in the buffer */
#define SNAP_CENSUS_BLOCK 32u        /* census granularity (one bit per block) */
#define SNAP_CENSUS_DUMP_EVERY 300   /* in-scope frames between census dumps */
#define SNAP_MAX_TICKS 16            /* the longest window (pads + inputs are recorded per tick) */
#define SNAP_PADS 4                  /* MAXCONTROLLERS */
#define SNAP_SE_RING 128
#define SNAP_DIFF_LOG_CAP 8          /* differing runs printed per MISS */
#define SNAP_WIDE_BLOCK 512u
#define SNAP_WIDE_LOG_CAP 48
#define SNAP_TICKS_PER_US (SYSCLOCK_ARM11 / 1000000ull)

/* ---- types ---------------------------------------------------------------------------------------- */

typedef struct SnapRegion {
    unsigned char* base;
    unsigned int size;
    const char* name;
} SnapRegion;

typedef struct SnapDynSlot {
    unsigned char* ptr;
    unsigned int size;
    const char* name;
} SnapDynSlot;

typedef struct SnapTickPads {
    unsigned short buttons[SNAP_PADS];
    signed char stickX[SNAP_PADS];
    signed char stickY[SNAP_PADS];
    unsigned char connected[SNAP_PADS];
    unsigned char have;
} SnapTickPads;

typedef struct SnapSeEvent {
    unsigned long long tick;
    unsigned short kind;
    unsigned short a;
    unsigned short b;
} SnapSeEvent;

typedef struct SnapWindow {
    int open;
    unsigned long long startTick;    /* T: the first tick the window covers */
    int ticks;                       /* N */
    int modeWord;
    int dynSerial;
    uint64_t saveTicks;
    unsigned int gstateAtSave;
} SnapWindow;

typedef struct SnapCounters {
    unsigned int windows;
    unsigned int pass;
    unsigned int miss;
    unsigned int skipped;
    unsigned int audioDropped;
    unsigned int seRecorded;
    unsigned int seDedup;
    unsigned int seNew;
} SnapCounters;

/* ---- state (the module's own; never in the snapshot) --------------------------------------------- */

static int sMode = SNAP_OFF;
static int sArmTried = 0;
static int sArmed = 0;
static int sRtMode = 0;
static int sResim = 0;
static int sDropNextTask = 0;

static SnapRegion sRegions[SNAP_MAX_REGIONS];
static int sRegionCount = 0;
static unsigned int sStaticBytes = 0;
static unsigned int sDataBlockBytes = 0;
static unsigned int sBssBlockBytes = 0;

/* snap-fast: the runtime NOSNAP holes (port/3ds/game/gdx3ds_snapshot_game.c's table): objects inside
 * the blocks that a race never writes, carved out of the regions so they are neither saved nor
 * restored. The gate watches each one (it must hold the same bytes at the window's save, at the
 * real T+N and after the resim) -- receipt `[snap] NOSNAP changed ... hole=<name>`. */
typedef struct SnapHole {
    unsigned char* base;
    unsigned int size;
    const char* name;
    unsigned char* copy; /* test only: the bytes at the window's save */
} SnapHole;
static SnapHole sHoles[SNAP_MAX_HOLES];
static int sHoleCount = 0;
static unsigned int sHoleBytes = 0;
static SnapDynSlot sDyn[GDX_SNAP_DYN_SLOTS];
static int sDynSerial = 0;
static unsigned int sDynCapacity = 0;

static unsigned char* sSaveBufs[SNAP_BUF_KINDS]; /* S per buffer kind (the probe A/Bs both) */
static unsigned char* sSaveBuf = NULL; /* S: the image at T (this window's kind) */
static unsigned char* sRealBuf = NULL; /* R (test only): the real image at T+N */
static unsigned int sBufBytes = 0;
static unsigned int sStaticSpan = 0;   /* the static regions' buffer span (cache-line aligned starts) */

/* snap-fast: how this window copies. Each lever defaults to -1 = cycle per window (so one run A/Bs
 * every variant and the gate covers every path); >= 0 pins it. */
typedef struct SnapVariant {
    int copy; /* GDX_SNAPCOPY_* */
    int buf;  /* SNAP_BUF_* */
    int sync; /* 0 = full copy, 1 = restore / resave only the blocks that differ */
} SnapVariant;
static SnapVariant sVar = { GDX_SNAPCOPY_PLD, SNAP_BUF_LINEAR, 0 };
static int sPinCopy = -1;
static int sPinBuf = -1;
static int sPinSync = -1;
static unsigned int sSyncWritten = 0;
static unsigned char* sNosnapCopy = NULL;

static SnapWindow sWin;
static SnapCounters sCnt;
static SnapTickPads sPads[SNAP_MAX_TICKS];
static int sTickIn[SNAP_MAX_TICKS][GDX_SNAP_IN_CHANNELS];
static unsigned char sTickInHave[SNAP_MAX_TICKS][GDX_SNAP_IN_CHANNELS];
static SnapSeEvent sSeRing[SNAP_SE_RING];
static unsigned int sSeHead = 0;

static unsigned long sNextFrame = 0;
static unsigned int sRng = 1u;
static int sGapMin = 30;
static int sGapMax = 150;
static int sTicksMax = 8;
static int sProbeEvery = 180;
static int sProbeTicks = 4;
static int sHeal = 1;
static int sWideWindows = 0;
static int sPrintEvery = 1;

static uint32_t* sWideReal = NULL;
static uint32_t* sWideResim = NULL;
static unsigned int sWideStaticBlocks = 0;
static unsigned int sWideArenaBlocks = 0;
static size_t sArenaBytes = 0;

/* ---- small helpers --------------------------------------------------------------------------------- */

static void SnapLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void SnapLog(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    const size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1u;
    gdx3ds_filelog_write(buf, len);
}

static unsigned int TicksToUs(uint64_t ticks) {
    return (unsigned int)(ticks / SNAP_TICKS_PER_US);
}

static unsigned int NextRandom(void) {
    sRng = sRng * 1103515245u + 12345u;
    return (sRng >> 8) & 0xFFFFFFu;
}

static int RandomInRange(int lo, int hi) {
    if (hi <= lo) {
        return lo;
    }
    return lo + (int)(NextRandom() % (unsigned int)(hi - lo + 1));
}

static uint32_t HashBytes(uint32_t h, const unsigned char* p, unsigned int n) {
    const unsigned int words = n / 4u;
    unsigned int i;
    for (i = 0; i < words; i++) {
        uint32_t w;
        memcpy(&w, p + i * 4u, 4u);
        h = (h ^ w) * 16777619u;
    }
    for (i = words * 4u; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static int IntConfig(const char* key, int fallback) {
    return gdx3ds_config_get_int("debug", key, fallback);
}

static int TickSlot(unsigned long long tick) {
    if (!sWin.open || tick < sWin.startTick) {
        return -1;
    }
    const unsigned long long rel = tick - sWin.startTick;
    if (rel >= (unsigned long long)sWin.ticks) {
        return -1;
    }
    return (int)rel;
}

/* The tick the game is simulating right now (the poll already advanced the counter). */
static int CurrentSimSlot(void) {
    const unsigned long long t = gdx_input_tick();
    return t == 0 ? -1 : TickSlot(t - 1u);
}

/* ---- regions --------------------------------------------------------------------------------------- */

static int RegionCovers(const SnapRegion* r, const unsigned char* p, unsigned int n) {
    return p >= r->base && p + n <= r->base + r->size;
}

static int CoveredByRegions(const void* ptr, unsigned int size) {
    int i;
    for (i = 0; i < sRegionCount; i++) {
        if (RegionCovers(&sRegions[i], (const unsigned char*)ptr, size)) {
            return 1;
        }
    }
    return 0;
}

static void AddRegion(unsigned char* base, unsigned int size, const char* name) {
    if (base == NULL || size == 0 || sRegionCount >= SNAP_MAX_REGIONS) {
        return;
    }
    sRegions[sRegionCount].base = base;
    sRegions[sRegionCount].size = size;
    sRegions[sRegionCount].name = name;
    sRegionCount++;
    sStaticBytes += size;
}

static int AddLinkerBlocks(void) {
    if (__gdx_snap_data_start == NULL || __gdx_snap_bss_start == NULL) {
        SnapLog("[snap] REFUSED: the snapshot linker script is not in this link (no __gdx_snap_* symbols)");
        return 0;
    }
    sDataBlockBytes = (unsigned int)(__gdx_snap_data_end - __gdx_snap_data_start);
    sBssBlockBytes = (unsigned int)(__gdx_snap_bss_end - __gdx_snap_bss_start);
    AddRegion(__gdx_snap_data_start, sDataBlockBytes, "data");
    AddRegion(__gdx_snap_bss_start, sBssBlockBytes, "bss");
    return 1;
}

/* Removes [lo, hi) from region i (which contains it): the region shrinks, splits in two, or goes. */
static void CarveFromRegion(int i, unsigned char* lo, unsigned char* hi) {
    SnapRegion r = sRegions[i];
    unsigned char* end = r.base + r.size;
    const int keepLeft = lo > r.base;
    const int keepRight = hi < end;
    int k;
    if (keepLeft && keepRight) {
        if (sRegionCount >= SNAP_MAX_REGIONS) {
            return;
        }
        for (k = sRegionCount; k > i + 1; k--) {
            sRegions[k] = sRegions[k - 1];
        }
        sRegionCount++;
        sRegions[i].size = (unsigned int)(lo - r.base);
        sRegions[i + 1].base = hi;
        sRegions[i + 1].size = (unsigned int)(end - hi);
        sRegions[i + 1].name = r.name;
    } else if (keepLeft) {
        sRegions[i].size = (unsigned int)(lo - r.base);
    } else if (keepRight) {
        sRegions[i].base = hi;
        sRegions[i].size = (unsigned int)(end - hi);
    } else {
        for (k = i; k + 1 < sRegionCount; k++) {
            sRegions[k] = sRegions[k + 1];
        }
        sRegionCount--;
    }
    sStaticBytes -= (unsigned int)(hi - lo);
}

static int FindRegionContaining(const unsigned char* lo, const unsigned char* hi) {
    int i;
    for (i = 0; i < sRegionCount; i++) {
        if (lo >= sRegions[i].base && hi <= sRegions[i].base + sRegions[i].size) {
            return i;
        }
    }
    return -1;
}

static int HoleOverlapsRequired(const unsigned char* lo, const unsigned char* hi) {
    const GdxSnapRequired* req = NULL;
    const int n = gdx3ds_snap_game_required(&req);
    int i;
    for (i = 0; i < n; i++) {
        const unsigned char* a = (const unsigned char*)req[i].ptr;
        if (a < hi && a + req[i].size > lo) {
            return 1;
        }
    }
    return 0;
}

/* [debug] snap_holes = 0 keeps card-4's full image (A/B); default 1. An entry not wholly inside one
 * region (absent from this build, already outside the blocks, overlapping a neighbour hole) is
 * skipped with a receipt rather than trusted. */
static void CarveHoles(void) {
    const GdxSnapRequired* t = NULL;
    const int wanted = IntConfig("snap_holes", 1) != 0 && sMode != SNAP_CENSUS; /* the census sees everything */
    const int n = wanted ? gdx3ds_snap_game_nosnap(&t) : 0;
    int i;
    for (i = 0; i < n && sHoleCount < SNAP_MAX_HOLES; i++) {
        unsigned char* lo = (unsigned char*)t[i].ptr;
        unsigned char* hi = lo + t[i].size;
        const int r = lo != NULL && t[i].size != 0 ? FindRegionContaining(lo, hi) : -1;
        if (r >= 0 && HoleOverlapsRequired(lo, hi)) {
            SnapLog("[snap] hole %s @%p overlaps a REQUIRED symbol -- not carved", t[i].name, (const void*)lo);
            continue;
        }
        if (r < 0) {
            if (lo != NULL) {
                SnapLog("[snap] hole %s @%p size=%u not inside one snapshot region -- not carved", t[i].name,
                        (const void*)lo, t[i].size);
            }
            continue;
        }
        CarveFromRegion(r, lo, hi);
        sHoles[sHoleCount].base = lo;
        sHoles[sHoleCount].size = t[i].size;
        sHoles[sHoleCount].name = t[i].name;
        sHoles[sHoleCount].copy = NULL;
        sHoleCount++;
        sHoleBytes += t[i].size;
        SnapLog("[snap] hole %s @%p size=%u", t[i].name, (const void*)lo, t[i].size);
    }
}

static void AddMissingRequired(void) {
    const GdxSnapRequired* req = NULL;
    const int n = gdx3ds_snap_game_required(&req);
    int i;
    for (i = 0; i < n; i++) {
        if (CoveredByRegions(req[i].ptr, req[i].size)) {
            continue;
        }
        SnapLog("[snap] REQUIRED %s @%p size=%u is outside the blocks -- covered as an extra region",
                req[i].name, req[i].ptr, req[i].size);
        AddRegion((unsigned char*)req[i].ptr, req[i].size, req[i].name);
    }
}

static unsigned int DynBytes(void) {
    unsigned int total = 0;
    int i;
    for (i = 0; i < GDX_SNAP_DYN_SLOTS; i++) {
        total += sDyn[i].size;
    }
    return total;
}

/* The dynamic slots' buffer span (each slot starts on a SNAP_ALIGN boundary). */
static unsigned int DynSpan(void) {
    unsigned int total = 0;
    int i;
    for (i = 0; i < GDX_SNAP_DYN_SLOTS; i++) {
        if (sDyn[i].size != 0) {
            total += (sDyn[i].size + SNAP_ALIGN - 1u) & ~(SNAP_ALIGN - 1u);
        }
    }
    return total;
}

static unsigned int StaticSpan(void) {
    unsigned int total = 0;
    int i;
    for (i = 0; i < sRegionCount; i++) {
        total += (sRegions[i].size + SNAP_ALIGN - 1u) & ~(SNAP_ALIGN - 1u);
    }
    return total;
}

/* ---- copy / compare -------------------------------------------------------------------------------- */

static unsigned int AlignUp(unsigned int v) {
    return (v + SNAP_ALIGN - 1u) & ~(SNAP_ALIGN - 1u);
}

/* Calls fn(region-or-slot base, size, name, buffer offset) for every region then every live
 * dynamic slot, in a fixed order: that order IS the buffer layout. Every entry starts on a
 * SNAP_ALIGN boundary of the buffer, so the copy engine's line bursts see aligned pairs. */
typedef void (*SnapVisitFn)(unsigned char* base, unsigned int size, const char* name, unsigned int off, void* ctx);

static unsigned int VisitAll(SnapVisitFn fn, void* ctx) {
    unsigned int off = 0;
    int i;
    for (i = 0; i < sRegionCount; i++) {
        fn(sRegions[i].base, sRegions[i].size, sRegions[i].name, off, ctx);
        off = AlignUp(off + sRegions[i].size);
    }
    for (i = 0; i < GDX_SNAP_DYN_SLOTS; i++) {
        if (sDyn[i].size == 0) {
            continue;
        }
        fn(sDyn[i].ptr, sDyn[i].size, sDyn[i].name, off, ctx);
        off = AlignUp(off + sDyn[i].size);
    }
    return off;
}

/* Live -> buffer (save) or buffer -> live (restore), with this window's variant: a full copy, or
 * (sync) only the blocks that differ. sSyncWritten counts the bytes a sync wrote. */
static void CopyWithVariant(unsigned char* dst, const unsigned char* src, unsigned int size, int sync) {
    if (sync) {
        sSyncWritten += gdx3ds_snapsync(dst, src, size, sVar.copy);
    } else {
        gdx3ds_snapcopy(dst, src, size, sVar.copy);
    }
}

static void VisitSave(unsigned char* base, unsigned int size, const char* name, unsigned int off, void* ctx) {
    (void)name;
    CopyWithVariant((unsigned char*)ctx + off, base, size, 0);
}

static void VisitSaveSync(unsigned char* base, unsigned int size, const char* name, unsigned int off, void* ctx) {
    (void)name;
    CopyWithVariant((unsigned char*)ctx + off, base, size, 1);
}

static void VisitRestore(unsigned char* base, unsigned int size, const char* name, unsigned int off, void* ctx) {
    (void)name;
    CopyWithVariant(base, (const unsigned char*)ctx + off, size, sVar.sync);
}

static void VisitHash(unsigned char* base, unsigned int size, const char* name, unsigned int off, void* ctx) {
    (void)name;
    (void)off;
    uint32_t* h = (uint32_t*)ctx;
    *h = HashBytes(*h, base, size);
}

typedef struct SnapDiffCtx {
    const unsigned char* real;
    unsigned int diffBytes;
    unsigned int runs;
    unsigned int logged;
} SnapDiffCtx;

static void LogDiffRun(const unsigned char* live, const unsigned char* real, unsigned int len, const char* name,
                       unsigned int regionOff) {
    uint32_t a = 0;
    uint32_t b = 0;
    memcpy(&a, real, len < 4u ? len : 4u);
    memcpy(&b, live, len < 4u ? len : 4u);
    SnapLog("[snap]   diff @%p len=%u region=%s+0x%x real=%08lx resim=%08lx", (const void*)live, len, name,
            regionOff, (unsigned long)a, (unsigned long)b);
}

/* A differing run ends after 8 equal bytes, so one changed struct reads as one run. */
static unsigned int DiffRunEnd(const unsigned char* live, const unsigned char* real, unsigned int from,
                               unsigned int size) {
    unsigned int lastDiff = from;
    unsigned int j = from + 1u;
    while (j < size && j - lastDiff <= 8u) {
        if (live[j] != real[j]) {
            lastDiff = j;
        }
        j++;
    }
    return lastDiff + 1u;
}

static void VisitDiff(unsigned char* base, unsigned int size, const char* name, unsigned int off, void* ctx) {
    SnapDiffCtx* d = (SnapDiffCtx*)ctx;
    const unsigned char* real = d->real + off;
    unsigned int i = 0;
    if (memcmp(base, real, size) == 0) {
        return;
    }
    while (i < size) {
        if (base[i] == real[i]) {
            i++;
            continue;
        }
        const unsigned int end = DiffRunEnd(base, real, i, size);
        d->diffBytes += end - i;
        d->runs++;
        if (d->logged < SNAP_DIFF_LOG_CAP) {
            LogDiffRun(base + i, real + i, end - i, name, i);
            d->logged++;
        }
        i = end;
    }
}

static void SaveImage(unsigned char* buf) {
    VisitAll(VisitSave, buf);
}

/* A save into a buffer that already holds a recent image: only the differing blocks are written. */
static unsigned int ResaveImage(unsigned char* buf) {
    sSyncWritten = 0;
    VisitAll(VisitSaveSync, buf);
    return sSyncWritten;
}

/* Returns the bytes written (the whole image for a full copy, the dirty blocks for a sync). */
static unsigned int RestoreImage(const unsigned char* buf) {
    sSyncWritten = 0;
    const unsigned int span = VisitAll(VisitRestore, (void*)buf);
    (void)span;
    return sVar.sync ? sSyncWritten : sStaticBytes + DynBytes();
}

static uint32_t HashImage(void) {
    uint32_t h = 2166136261u;
    VisitAll(VisitHash, &h);
    return h;
}

/* ---- the nosnap watch (test only) ------------------------------------------------------------------ */

static unsigned int NosnapBytes(void) {
    if (__gdx_nosnap_bss_start == NULL) {
        return 0;
    }
    return (unsigned int)(__gdx_nosnap_bss_end - __gdx_nosnap_bss_start);
}

static void NosnapSave(void) {
    if (sNosnapCopy != NULL) {
        memcpy(sNosnapCopy, __gdx_nosnap_bss_start, NosnapBytes());
    }
}

static void NosnapCheck(void) {
    const unsigned int n = NosnapBytes();
    unsigned int i;
    int logged = 0;
    if (sNosnapCopy == NULL || memcmp(sNosnapCopy, __gdx_nosnap_bss_start, n) == 0) {
        return;
    }
    for (i = 0; i < n && logged < 4; i += 64u) {
        const unsigned int len = (n - i) < 64u ? (n - i) : 64u;
        if (memcmp(sNosnapCopy + i, __gdx_nosnap_bss_start + i, len) != 0) {
            SnapLog("[snap] NOSNAP changed in a race window: @%p (nosnap+0x%x) -- not static, review its exclusion",
                    (void*)(__gdx_nosnap_bss_start + i), i);
            logged++;
        }
    }
}

/* ---- the hole watch (test only) ------------------------------------------------------------------- */

static void HolesAllocate(void) {
    int i;
    for (i = 0; i < sHoleCount; i++) {
        sHoles[i].copy = (unsigned char*)malloc(sHoles[i].size);
    }
}

static void HolesSave(void) {
    int i;
    for (i = 0; i < sHoleCount; i++) {
        if (sHoles[i].copy != NULL) {
            memcpy(sHoles[i].copy, sHoles[i].base, sHoles[i].size);
        }
    }
}

/* Returns the number of holes that differ from the window's save. */
static int HolesCheck(const char* stage) {
    int changed = 0;
    int i;
    for (i = 0; i < sHoleCount; i++) {
        const SnapHole* h = &sHoles[i];
        unsigned int o;
        if (h->copy == NULL || memcmp(h->copy, h->base, h->size) == 0) {
            continue;
        }
        for (o = 0; o < h->size && h->copy[o] == h->base[o]; o++) {
        }
        SnapLog("[snap] NOSNAP changed in a race window: hole=%s @%p (+0x%x) stage=%s -- not static, review its "
                "exclusion", h->name, (void*)(h->base + o), o, stage);
        changed++;
    }
    return changed;
}

/* ---- the wide census (test, snap_wide = K) -------------------------------------------------------- */

static unsigned int WideStaticBytes(void) {
    return (unsigned int)(__end__ - __gdx_snap_data_start);
}

static void WideHashAll(uint32_t* out) {
    unsigned int b;
    const unsigned char* base = __gdx_snap_data_start;
    const unsigned int staticBytes = WideStaticBytes();
    for (b = 0; b < sWideStaticBlocks; b++) {
        const unsigned int off = b * SNAP_WIDE_BLOCK;
        const unsigned int len = (staticBytes - off) < SNAP_WIDE_BLOCK ? (staticBytes - off) : SNAP_WIDE_BLOCK;
        out[b] = HashBytes(2166136261u, base + off, len);
    }
    for (b = 0; b < sWideArenaBlocks; b++) {
        out[sWideStaticBlocks + b] = HashBytes(2166136261u, gdx_rdram + (size_t)b * SNAP_WIDE_BLOCK, SNAP_WIDE_BLOCK);
    }
}

static int WideBlockInsideSnapshot(const unsigned char* p) {
    return CoveredByRegions(p, SNAP_WIDE_BLOCK);
}

static const unsigned char* WideBlockAddr(unsigned int b) {
    if (b < sWideStaticBlocks) {
        return __gdx_snap_data_start + b * SNAP_WIDE_BLOCK;
    }
    return gdx_rdram + (size_t)(b - sWideStaticBlocks) * SNAP_WIDE_BLOCK;
}

static void WideReport(unsigned int windowIndex) {
    const unsigned int total = sWideStaticBlocks + sWideArenaBlocks;
    unsigned int b = 0;
    int logged = 0;
    unsigned int outside = 0;
    while (b < total) {
        if (sWideReal[b] == sWideResim[b] || WideBlockInsideSnapshot(WideBlockAddr(b))) {
            b++;
            continue;
        }
        unsigned int e = b;
        while (e < total && sWideReal[e] != sWideResim[e] && !WideBlockInsideSnapshot(WideBlockAddr(e)) &&
               (e == b || WideBlockAddr(e) == WideBlockAddr(e - 1) + SNAP_WIDE_BLOCK)) {
            e++;
        }
        outside += e - b;
        if (logged < SNAP_WIDE_LOG_CAP) {
            const int inArena = b >= sWideStaticBlocks;
            SnapLog("[snap] WIDE #%u outside-snapshot change @%p len=%u (%s+0x%x)", windowIndex,
                    (const void*)WideBlockAddr(b), (e - b) * SNAP_WIDE_BLOCK, inArena ? "rdram" : "static",
                    inArena ? (b - sWideStaticBlocks) * SNAP_WIDE_BLOCK : b * SNAP_WIDE_BLOCK);
            logged++;
        }
        b = e;
    }
    SnapLog("[snap] WIDE #%u done: %u outside-snapshot blocks of %u bytes differ between real and resim", windowIndex,
            outside, SNAP_WIDE_BLOCK);
}

static void WideAllocate(void) {
    size_t stats[10];
    memset(stats, 0, sizeof(stats));
    gdx_rdram_probe_stats(stats);
    sArenaBytes = stats[0];
    sWideStaticBlocks = (WideStaticBytes() + SNAP_WIDE_BLOCK - 1u) / SNAP_WIDE_BLOCK;
    sWideArenaBlocks = (unsigned int)(sArenaBytes / SNAP_WIDE_BLOCK);
    const size_t bytes = (size_t)(sWideStaticBlocks + sWideArenaBlocks) * sizeof(uint32_t);
    sWideReal = (uint32_t*)malloc(bytes);
    sWideResim = (uint32_t*)malloc(bytes);
    if (sWideReal == NULL || sWideResim == NULL) {
        SnapLog("[snap] snap_wide: %u bytes of block tables unavailable -- wide census off", (unsigned int)(bytes * 2u));
        free(sWideReal);
        free(sWideResim);
        sWideReal = NULL;
        sWideResim = NULL;
        sWideWindows = 0;
    }
}

/* ---- the dirty census (snap-fast, [debug] snap_census = 1) ------------------------------------------ */

/* Which parts of the static image ever change during a race. Every in-scope frame each
 * SNAP_CENSUS_BLOCK block is compared with its value one frame earlier (kept in the save buffer);
 * a change sets the block's bit for the rest of the session. Re-baselined, unmarked, at every scope
 * entry: a course load between races is not a change IN a race. tools/dev/snapcensus.py maps the
 * dumped bitmap onto the ELF's symbols -- a symbol whose every block stayed clean across the whole
 * replay corpus is a candidate for the NOSNAP class (not restored, watched by the gate). */
static uint32_t* sCensusMap = NULL;
static unsigned int sCensusBlocks = 0;
static int sCensusLive = 0;
static unsigned int sCensusFrames = 0;
static unsigned int sCensusRaces = 0;
static unsigned int sCensusDumps = 0;
static uint64_t sCensusDirtySum = 0;
static unsigned int sCensusDirtyMax = 0;

static unsigned int CensusRegionBlocks(unsigned int size) {
    return (size + SNAP_CENSUS_BLOCK - 1u) / SNAP_CENSUS_BLOCK;
}

static int CensusAllocate(void) {
    int i;
    sCensusBlocks = 0;
    for (i = 0; i < sRegionCount; i++) {
        sCensusBlocks += CensusRegionBlocks(sRegions[i].size);
    }
    sCensusMap = (uint32_t*)calloc((sCensusBlocks + 31u) / 32u, sizeof(uint32_t));
    return sCensusMap != NULL;
}

/* Compares every block with last frame's copy (and refreshes the copy). mark = 0 only baselines. */
static unsigned int CensusScan(int mark) {
    unsigned int off = 0;
    unsigned int block = 0;
    unsigned int dirty = 0;
    int i;
    for (i = 0; i < sRegionCount; i++) {
        const unsigned char* live = sRegions[i].base;
        unsigned char* prev = sSaveBuf + off;
        unsigned int o;
        for (o = 0; o < sRegions[i].size; o += SNAP_CENSUS_BLOCK, block++) {
            const unsigned int left = sRegions[i].size - o;
            const unsigned int len = left < SNAP_CENSUS_BLOCK ? left : SNAP_CENSUS_BLOCK;
            if (memcmp(live + o, prev + o, len) == 0) {
                continue;
            }
            memcpy(prev + o, live + o, len);
            if (mark) {
                sCensusMap[block / 32u] |= 1u << (block % 32u);
                dirty++;
            }
        }
        off = AlignUp(off + sRegions[i].size);
    }
    return dirty;
}

static unsigned int CensusDirtyTotal(void) {
    unsigned int n = 0;
    unsigned int b;
    for (b = 0; b < sCensusBlocks; b++) {
        n += (sCensusMap[b / 32u] >> (b % 32u)) & 1u;
    }
    return n;
}

#define SNAP_CENSUS_WORDS_PER_LINE 24u

static void CensusDump(const char* why) {
    const unsigned int words = (sCensusBlocks + 31u) / 32u;
    unsigned int first = 0;
    unsigned int w;
    int i;
    sCensusDumps++;
    SnapLog("[snap] census #%u (%s) frames=%u races=%u dirty=%u/%u blocks of %u B, per-frame avg=%u max=%u blocks",
            sCensusDumps, why, sCensusFrames, sCensusRaces, CensusDirtyTotal(), sCensusBlocks, SNAP_CENSUS_BLOCK,
            sCensusFrames != 0 ? (unsigned int)(sCensusDirtySum / sCensusFrames) : 0u, sCensusDirtyMax);
    for (i = 0; i < sRegionCount; i++) {
        SnapLog("[snap] cregion %d %s base=%p size=%u first_block=%u", i, sRegions[i].name, (void*)sRegions[i].base,
                sRegions[i].size, first);
        first += CensusRegionBlocks(sRegions[i].size);
    }
    for (w = 0; w < words; w += SNAP_CENSUS_WORDS_PER_LINE) {
        char line[SNAP_CENSUS_WORDS_PER_LINE * 8u + 1u];
        unsigned int k;
        unsigned int len = 0;
        for (k = 0; k < SNAP_CENSUS_WORDS_PER_LINE && w + k < words; k++) {
            len += (unsigned int)snprintf(line + len, sizeof(line) - len, "%08lx", (unsigned long)sCensusMap[w + k]);
        }
        line[len] = '\0';
        SnapLog("[snap] cmap %u %s", w * 32u, line);
    }
}

static void CensusFrame(void) {
    if (!gdx3ds_snap_game_scope()) {
        if (sCensusLive) {
            sCensusLive = 0;
            CensusDump("scope left");
        }
        return;
    }
    if (!sCensusLive) {
        (void)CensusScan(0);
        sCensusLive = 1;
        sCensusRaces++;
        return;
    }
    const unsigned int dirty = CensusScan(1);
    sCensusFrames++;
    sCensusDirtySum += dirty;
    if (dirty > sCensusDirtyMax) {
        sCensusDirtyMax = dirty;
    }
    if ((sCensusFrames % SNAP_CENSUS_DUMP_EVERY) == 0) {
        CensusDump("periodic");
    }
}

/* ---- arming ---------------------------------------------------------------------------------------- */

/* card-4 allocated every buffer with linearAlloc. libctru's linear heap is the LINEAR (physically
 * contiguous, GPU-visible) FCRAM mapping; the app maps it normal cacheable like the regular heap
 * (that is why GPU uploads need GSPGPU_FlushDataCache), so no difference is expected -- but the
 * probe A/Bs it on the console rather than assume. Both are SNAP_ALIGN aligned. */
static unsigned char* AllocKind(unsigned int bytes, int kind) {
    if (kind == SNAP_BUF_HEAP) {
        return (unsigned char*)memalign(SNAP_ALIGN, bytes);
    }
    return (unsigned char*)linearMemAlign(bytes, SNAP_ALIGN);
}

static unsigned char* AllocBuffer(unsigned int bytes) {
    const int kind = sPinBuf == SNAP_BUF_HEAP ? SNAP_BUF_HEAP : SNAP_BUF_LINEAR;
    unsigned char* p = AllocKind(bytes, kind);
    if (p == NULL) {
        p = AllocKind(bytes, kind == SNAP_BUF_HEAP ? SNAP_BUF_LINEAR : SNAP_BUF_HEAP);
    }
    return p;
}

static void ReadLevers(void) {
    sGapMin = IntConfig("snap_gap_min", 30);
    sGapMax = IntConfig("snap_gap_max", 150);
    sTicksMax = IntConfig("snap_n_max", 8);
    if (sTicksMax < 1) {
        sTicksMax = 1;
    } else if (sTicksMax > SNAP_MAX_TICKS) {
        sTicksMax = SNAP_MAX_TICKS;
    }
    sProbeEvery = IntConfig("snap_probe_every", 180);
    sProbeTicks = IntConfig("snap_probe_ticks", 4);
    if (sProbeTicks < 1) {
        sProbeTicks = 1;
    } else if (sProbeTicks > SNAP_MAX_TICKS) {
        sProbeTicks = SNAP_MAX_TICKS;
    }
    sHeal = IntConfig("snap_heal", 1);
    sWideWindows = IntConfig("snap_wide", 0);
    sPrintEvery = IntConfig("snap_print_every", 1);
    if (sPrintEvery < 1) {
        sPrintEvery = 1;
    }
    sRng = (unsigned int)IntConfig("snap_seed", 1);
    sPinCopy = IntConfig("snap_copy", -1);
    if (sPinCopy >= GDX_SNAPCOPY_MODES) {
        sPinCopy = GDX_SNAPCOPY_PLD;
    }
    sPinBuf = IntConfig("snap_buf", -1);
    if (sPinBuf >= SNAP_BUF_KINDS) {
        sPinBuf = SNAP_BUF_LINEAR;
    }
    sPinSync = IntConfig("snap_sync", -1);
    if (sPinSync > 1) {
        sPinSync = 1;
    }
}

/* ---- variants (snap-fast) -------------------------------------------------------------------------- */

static int VariantBufCount(int* kinds) {
    int n = 0;
    int kind;
    for (kind = 0; kind < SNAP_BUF_KINDS; kind++) {
        if (sSaveBufs[kind] != NULL) {
            kinds[n++] = kind;
        }
    }
    return n;
}

/* Window k's variant: a mixed-radix walk over every unpinned lever, so consecutive windows visit
 * every combination (copy fastest, then sync, then buffer kind). */
static SnapVariant ChooseVariant(unsigned int k) {
    SnapVariant v;
    int kinds[SNAP_BUF_KINDS];
    const unsigned int nCopy = sPinCopy >= 0 ? 1u : (unsigned int)GDX_SNAPCOPY_MODES;
    const unsigned int nSync = sPinSync >= 0 ? 1u : 2u;
    const unsigned int nBuf = (unsigned int)VariantBufCount(kinds);
    v.copy = sPinCopy >= 0 ? sPinCopy : (int)(k % nCopy);
    k /= nCopy;
    v.sync = sPinSync >= 0 ? sPinSync : (int)(k % nSync);
    k /= nSync;
    v.buf = kinds[nBuf > 0 ? k % nBuf : 0];
    return v;
}

static const char* BufKindName(int kind) {
    return kind == SNAP_BUF_HEAP ? "heap" : "linear";
}

/* The probe with snap_buf = -1 holds one save buffer of EACH kind and alternates; otherwise one
 * buffer of the pinned kind (linear by default, card-4's). A kind that will not allocate drops
 * out of the cycle. */
static void AllocateSaveBuffers(void) {
    int kind;
    for (kind = 0; kind < SNAP_BUF_KINDS; kind++) {
        const int wanted = sPinBuf < 0 ? (sMode == SNAP_PROBE || kind == SNAP_BUF_LINEAR) : kind == sPinBuf;
        sSaveBufs[kind] = wanted ? AllocKind(sBufBytes, kind) : NULL;
    }
    if (sSaveBufs[SNAP_BUF_LINEAR] == NULL && sSaveBufs[SNAP_BUF_HEAP] == NULL) {
        sSaveBufs[SNAP_BUF_HEAP] = AllocBuffer(sBufBytes);
    }
    sSaveBuf = sSaveBufs[SNAP_BUF_LINEAR] != NULL ? sSaveBufs[SNAP_BUF_LINEAR] : sSaveBufs[SNAP_BUF_HEAP];
}

static int AllocateBuffers(void) {
    sDynCapacity = 16u * 1024u + GDX_SNAP_DYN_SLOTS * SNAP_ALIGN;
    sStaticSpan = StaticSpan();
    sBufBytes = sStaticSpan + sDynCapacity;
    AllocateSaveBuffers();
    if (sMode == SNAP_TEST) {
        sRealBuf = AllocBuffer(sBufBytes);
        sNosnapCopy = NosnapBytes() != 0 ? AllocBuffer(NosnapBytes()) : NULL;
    }
    if (sSaveBuf == NULL || (sMode == SNAP_TEST && sRealBuf == NULL)) {
        SnapLog("[snap] REFUSED: %u-byte snapshot buffer unavailable", sBufBytes);
        return 0;
    }
    return 1;
}

static void LogLayout(void) {
    SnapLog("[snap] init mode=%s regions=%d bytes=%u data=%u bss=%u extra=%u dyn_cap=%u nosnap=%u buffers=%u "
            "copy=%d sync=%d buf=%s%s%s (-1 = cycled per window)",
            sMode == SNAP_TEST ? "test" : (sMode == SNAP_CENSUS ? "census" : "probe"), sRegionCount, sStaticBytes,
            sDataBlockBytes, sBssBlockBytes, sStaticBytes + sHoleBytes - sDataBlockBytes - sBssBlockBytes, sDynCapacity,
            NosnapBytes(), sBufBytes * (sMode == SNAP_TEST ? 2u : 1u), sPinCopy, sPinSync,
            sSaveBufs[SNAP_BUF_LINEAR] != NULL ? "linear" : "", sSaveBufs[SNAP_BUF_LINEAR] != NULL && sSaveBufs[SNAP_BUF_HEAP] != NULL ? "+" : "",
            sSaveBufs[SNAP_BUF_HEAP] != NULL ? "heap" : "");
    SnapLog("[snap] holes=%d hole_bytes=%u (runtime NOSNAP, [debug] snap_holes; bytes above are after the holes)",
            sHoleCount, sHoleBytes);
    SnapLog("[snap] layout data=%p+%u bss=%p+%u nosnap=%p+%u (gap %s)", (void*)__gdx_snap_data_start, sDataBlockBytes,
            (void*)__gdx_snap_bss_start, sBssBlockBytes, (void*)__gdx_nosnap_bss_start, NosnapBytes(),
            sMode == SNAP_TEST ? "snap_gap_min..max frames, N = 1..snap_n_max" : "snap_probe_every frames");
}

static void Arm(void) {
    sArmTried = 1;
    if (!AddLinkerBlocks()) {
        sMode = SNAP_OFF;
        return;
    }
    AddMissingRequired();
    CarveHoles();
    ReadLevers();
    if (!AllocateBuffers()) {
        sMode = SNAP_OFF;
        return;
    }
    if (sMode == SNAP_TEST && sWideWindows > 0) {
        WideAllocate();
    }
    if (sMode == SNAP_TEST) {
        HolesAllocate();
    }
    if (sMode == SNAP_CENSUS && !CensusAllocate()) {
        SnapLog("[snap] REFUSED: census bitmap unavailable");
        sMode = SNAP_OFF;
        return;
    }
    LogLayout();
    sArmed = 1;
}

static int ResolveMode(void) {
    if (IntConfig("snap_census", 0) != 0) {
        return SNAP_CENSUS;
    }
    if (IntConfig("snap_test", 0) != 0) {
        return SNAP_TEST;
    }
    if (IntConfig("snap_probe", 0) != 0) {
        return SNAP_PROBE;
    }
    return SNAP_OFF;
}

/* ---- hooks (port/gdx_snapshot.h) ------------------------------------------------------------------- */

int gdx_snap_resim_active(void) {
    return sResim;
}

void gdx_snap_note_pads(unsigned long long tick, const unsigned short* buttons, const signed char* stickX,
                        const signed char* stickY, const unsigned char* connected, int count) {
    const int slot = TickSlot(tick);
    int i;
    if (slot < 0 || sResim) {
        return;
    }
    SnapTickPads* p = &sPads[slot];
    for (i = 0; i < SNAP_PADS; i++) {
        p->buttons[i] = i < count ? buttons[i] : 0;
        p->stickX[i] = i < count ? stickX[i] : 0;
        p->stickY[i] = i < count ? stickY[i] : 0;
        p->connected[i] = i < count ? connected[i] : 0;
    }
    p->have = 1;
}

int gdx_snap_recorded_pads(unsigned long long tick, unsigned short* buttons, signed char* stickX, signed char* stickY,
                           unsigned char* connected, int count) {
    const int slot = TickSlot(tick);
    int i;
    if (slot < 0 || !sPads[slot].have) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        buttons[i] = i < SNAP_PADS ? sPads[slot].buttons[i] : 0;
        stickX[i] = i < SNAP_PADS ? sPads[slot].stickX[i] : 0;
        stickY[i] = i < SNAP_PADS ? sPads[slot].stickY[i] : 0;
        connected[i] = i < SNAP_PADS ? sPads[slot].connected[i] : 0;
    }
    return 1;
}

int gdx_snap_tick_input(int channel, int value) {
    if (!sWin.open || channel < 0 || channel >= GDX_SNAP_IN_CHANNELS) {
        return value;
    }
    const int slot = CurrentSimSlot();
    if (slot < 0) {
        return value;
    }
    if (sResim) {
        return sTickInHave[slot][channel] ? sTickIn[slot][channel] : value;
    }
    sTickIn[slot][channel] = value;
    sTickInHave[slot][channel] = 1;
    return value;
}

int gdx_snap_drop_task(void) {
    return sDropNextTask;
}

int gdx_snap_audio_drop(void) {
    if (!sResim || !gdx_sched_on_host_thread()) {
        return 0;
    }
    sCnt.audioDropped++;
    return 1;
}

static int SeRingContains(unsigned long long tick, int kind, int a, int b) {
    unsigned int i;
    for (i = 0; i < SNAP_SE_RING; i++) {
        const SnapSeEvent* e = &sSeRing[i];
        if (e->tick == tick && e->kind == (unsigned short)kind && e->a == (unsigned short)a &&
            e->b == (unsigned short)b) {
            return 1;
        }
    }
    return 0;
}

static void SeRingPush(unsigned long long tick, int kind, int a, int b) {
    SnapSeEvent* e = &sSeRing[sSeHead % SNAP_SE_RING];
    e->tick = tick;
    e->kind = (unsigned short)kind;
    e->a = (unsigned short)a;
    e->b = (unsigned short)b;
    sSeHead++;
}

int gdx_snap_se_gate(int kind, int a, int b) {
    if (!sWin.open || !gdx_sched_on_host_thread()) {
        return 0;
    }
    const unsigned long long tick = gdx_input_tick();
    if (!sResim) {
        SeRingPush(tick, kind, a, b);
        sCnt.seRecorded++;
        return 0;
    }
    if (SeRingContains(tick, kind, a, b)) {
        sCnt.seDedup++;
    } else {
        sCnt.seNew++; /* a real rollback would play it now; the probe's timeline is the real one */
    }
    return 1;
}

void gdx_snap_register_dynamic(int slot, void* ptr, unsigned int size, const char* name) {
    if (slot < 0 || slot >= GDX_SNAP_DYN_SLOTS) {
        return;
    }
    sDyn[slot].ptr = (unsigned char*)ptr;
    sDyn[slot].size = ptr != NULL ? size : 0u;
    sDyn[slot].name = name;
    sDynSerial++;
}

/* ---- the window ------------------------------------------------------------------------------------ */

static void ClearWindowRecords(void) {
    memset(sPads, 0, sizeof(sPads));
    memset(sTickInHave, 0, sizeof(sTickInHave));
    memset(sSeRing, 0, sizeof(sSeRing));
    sSeHead = 0;
}

static int WindowEligible(void) {
    if (!gdx3ds_snap_game_scope()) {
        return 0;
    }
    if (DynSpan() > sDynCapacity) {
        return 0;
    }
    if (sMode == SNAP_PROBE) {
        const int session = (&gdx3ds_mp_active != 0) && gdx3ds_mp_active();
        return gdx3ds_snap_game_single_player() && !session;
    }
    return 1;
}

static int ChooseWindowTicks(void) {
    return sMode == SNAP_PROBE ? sProbeTicks : RandomInRange(1, sTicksMax);
}

static unsigned long ChooseGap(void) {
    return sMode == SNAP_PROBE ? (unsigned long)sProbeEvery : (unsigned long)RandomInRange(sGapMin, sGapMax);
}

static void OpenWindow(void) {
    ClearWindowRecords();
    sVar = ChooseVariant(sCnt.windows);
    sSaveBuf = sSaveBufs[sVar.buf];
    sWin.ticks = ChooseWindowTicks();
    sWin.startTick = gdx_input_tick();
    sWin.modeWord = gdx3ds_snap_game_mode_word();
    sWin.dynSerial = sDynSerial;
    sWin.gstateAtSave = gdx_gstate_hash(NULL);
    const uint64_t t0 = svcGetSystemTick();
    SaveImage(sSaveBuf);
    sWin.saveTicks = svcGetSystemTick() - t0;
    if (sMode == SNAP_TEST) {
        NosnapSave();
        HolesSave();
    }
    sWin.open = 1;
}

static void CloseWindow(unsigned long frame) {
    sWin.open = 0;
    sNextFrame = frame + ChooseGap();
}

static void JoinRenderThread(void) {
    if (sRtMode != 0) {
        gdx3ds_rt_join_idle();
    }
}

/* One silent tick: the host loop's poll + VI + dispatch, nothing else (no APT pump, no frame
 * begin/end, no present, no audio notify, no save tick, no oracle). The GFX task is acked undrawn. */
static int ResimOneTick(void) {
    if (!gdx_controller_poll_resim()) {
        return 0;
    }
    sDropNextTask = 1;
    gdx_vi_tick();
    gdx_sched_drain_deferred_wakes();
    gdx_dispatch();
    while (sRtMode != 0 && gdx3ds_rt_task_pending()) {
        gdx3ds_rt_wait_task();
        gdx_sched_drain_deferred_wakes();
        gdx_dispatch();
    }
    sDropNextTask = 0;
    return 1;
}

static int ResimTicks(int n) {
    int i;
    int ok = 1;
    sResim = 1;
    for (i = 0; i < n && ok; i++) {
        ok = ResimOneTick();
    }
    sResim = 0;
    return ok;
}

typedef struct SnapTimes {
    unsigned int saveUs;
    unsigned int restoreUs;
    unsigned int resimUs;
    unsigned int hashUs;
    unsigned int restoreBytes; /* written by the restore (the dirty blocks for a sync) */
    unsigned int resaveUs;     /* probe: a sync save of T+N over the T image (run-ahead's next save) */
    unsigned int resaveBytes;
} SnapTimes;

static unsigned int MegabytesPerSecond(unsigned int bytes, unsigned int us) {
    return us == 0 ? 0u : (unsigned int)(((uint64_t)bytes * 1000000ull / (uint64_t)us) >> 20);
}

static unsigned int RestoreAndResim(SnapTimes* t, int* resimOk) {
    uint64_t t0 = svcGetSystemTick();
    t->restoreBytes = RestoreImage(sSaveBuf);
    t->restoreUs = TicksToUs(svcGetSystemTick() - t0);
    t0 = svcGetSystemTick();
    *resimOk = ResimTicks(sWin.ticks);
    t->resimUs = TicksToUs(svcGetSystemTick() - t0);
    return gdx_gstate_hash(NULL);
}

static void LogMiss(const SnapDiffCtx* d, unsigned int windowIndex) {
    SnapLog("[snap] MISS #%u tick=%llu n=%d diff_bytes=%u runs=%u (first %u above) -- %s", windowIndex,
            sWin.startTick, sWin.ticks, d->diffBytes, d->runs, d->logged,
            sHeal ? "healed from the real image" : "NOT healed (snap_heal=0)");
}

static void FinishTestWindow(void) {
    SnapTimes t;
    SnapDiffCtx d;
    int resimOk = 0;
    const unsigned int windowIndex = ++sCnt.windows;
    const int wide = sWideReal != NULL && (int)windowIndex <= sWideWindows;
    memset(&t, 0, sizeof(t));
    memset(&d, 0, sizeof(d));
    t.saveUs = TicksToUs(sWin.saveTicks);
    JoinRenderThread();
    NosnapCheck();
    const int holesReal = HolesCheck("real");
    const unsigned int realGstate = gdx_gstate_hash(NULL);
    if (sVar.sync) {
        (void)ResaveImage(sRealBuf); /* R holds the last window's real image: exercises the sync save */
    } else {
        SaveImage(sRealBuf);
    }
    if (wide) {
        WideHashAll(sWideReal);
    }
    const unsigned int resimGstate = RestoreAndResim(&t, &resimOk);
    const int holesResim = HolesCheck("resim");
    const uint64_t c0 = svcGetSystemTick();
    d.real = sRealBuf;
    VisitAll(VisitDiff, &d);
    t.hashUs = TicksToUs(svcGetSystemTick() - c0);
    if (wide) {
        WideHashAll(sWideResim);
        WideReport(windowIndex);
    }
    const int ok = resimOk && d.runs == 0 && holesReal == 0 && holesResim == 0;
    if (ok) {
        sCnt.pass++;
    } else {
        sCnt.miss++;
        LogMiss(&d, windowIndex);
        if (sHeal) {
            RestoreImage(sRealBuf);
        }
    }
    if (!ok || (windowIndex % (unsigned int)sPrintEvery) == 0) {
        SnapLog("[snap] test #%u tick=%llu n=%d ok=%d bytes=%u var=%s/%s/%s restored=%u save_us=%u restore_us=%u "
                "resim_us=%u cmp_us=%u gstate=%08x/%08x pass=%u miss=%u skip=%u acmd_drop=%u se=%u/%u/%u",
                windowIndex, sWin.startTick, sWin.ticks, ok, sStaticBytes + DynBytes(), gdx3ds_snapcopy_mode_name(sVar.copy),
                sVar.sync ? "sync" : "full", BufKindName(sVar.buf), t.restoreBytes, t.saveUs, t.restoreUs, t.resimUs,
                t.hashUs, realGstate, resimGstate, sCnt.pass, sCnt.miss, sCnt.skipped, sCnt.audioDropped,
                sCnt.seRecorded, sCnt.seDedup, sCnt.seNew);
    }
}

static void FinishProbeWindow(void) {
    SnapTimes t;
    int resimOk = 0;
    const unsigned int windowIndex = ++sCnt.windows;
    const unsigned int bytes = sStaticBytes + DynBytes();
    const unsigned int dropBefore = sCnt.audioDropped;
    const unsigned int dedupBefore = sCnt.seDedup;
    const unsigned int newBefore = sCnt.seNew;
    memset(&t, 0, sizeof(t));
    t.saveUs = TicksToUs(sWin.saveTicks);
    JoinRenderThread();
    uint64_t h0 = svcGetSystemTick();
    const uint32_t realHash = HashImage();
    t.hashUs = TicksToUs(svcGetSystemTick() - h0);
    (void)RestoreAndResim(&t, &resimOk);
    const uint32_t resimHash = HashImage();
    const int ok = resimOk && realHash == resimHash;
    const uint64_t r0 = svcGetSystemTick();
    t.resaveBytes = ResaveImage(sSaveBuf);
    t.resaveUs = TicksToUs(svcGetSystemTick() - r0);
    if (ok) {
        sCnt.pass++;
    } else {
        sCnt.miss++;
    }
    SnapLog("[snap] probe #%u tick=%llu n=%d ok=%d bytes=%u var=%s/%s/%s save_us=%u restore_us=%u restored=%u "
            "resave_us=%u resaved=%u resim_us=%u per_tick_us=%u hash_us=%u copy_mbps=%u acmd_drop=%u se_dedup=%u "
            "se_new=%u pass=%u miss=%u",
            windowIndex, sWin.startTick, sWin.ticks, ok, bytes, gdx3ds_snapcopy_mode_name(sVar.copy),
            sVar.sync ? "sync" : "full", BufKindName(sVar.buf), t.saveUs, t.restoreUs, t.restoreBytes, t.resaveUs,
            t.resaveBytes, t.resimUs, t.resimUs / (unsigned int)sWin.ticks, t.hashUs,
            MegabytesPerSecond(bytes, t.saveUs), sCnt.audioDropped - dropBefore, sCnt.seDedup - dedupBefore,
            sCnt.seNew - newBefore, sCnt.pass, sCnt.miss);
}

static int WindowStillValid(void) {
    return gdx3ds_snap_game_scope() && gdx3ds_snap_game_mode_word() == sWin.modeWord &&
           sWin.dynSerial == sDynSerial;
}

static void SkipWindow(unsigned long frame, const char* why) {
    sCnt.skipped++;
    if (sMode == SNAP_TEST) {
        SnapLog("[snap] skip tick=%llu n=%d: %s (the real run continues, nothing restored)", sWin.startTick,
                sWin.ticks, why);
    }
    CloseWindow(frame);
}

static void AdvanceOpenWindow(unsigned long frame) {
    const unsigned long long done = gdx_input_tick() - sWin.startTick;
    if (!WindowStillValid()) {
        SkipWindow(frame, "mode change / scope left / dynamic region re-registered");
        return;
    }
    if (done < (unsigned long long)sWin.ticks) {
        return;
    }
    if (done > (unsigned long long)sWin.ticks) {
        SkipWindow(frame, "more ticks than polls (divider)");
        return;
    }
    if (sMode == SNAP_TEST) {
        FinishTestWindow();
    } else {
        FinishProbeWindow();
    }
    CloseWindow(frame);
}

static void MaybeOpenWindow(unsigned long frame) {
    if (frame < sNextFrame || !WindowEligible()) {
        return;
    }
    OpenWindow();
}

void gdx3ds_snapshot_frame_end(unsigned long frame, int rtMode) {
    if (!sArmTried) {
        sMode = ResolveMode();
        if (sMode == SNAP_OFF) {
            sArmTried = 1;
            return;
        }
        sRtMode = rtMode;
        Arm();
        sNextFrame = frame + ChooseGap();
    }
    if (!sArmed) {
        return;
    }
    if (sMode == SNAP_CENSUS) {
        CensusFrame();
        return;
    }
    if (sWin.open) {
        AdvanceOpenWindow(frame);
    } else {
        MaybeOpenWindow(frame);
    }
}
