/* port/3ds/gdx3ds_hwprof.h -- hwprof: hardware attribution instruments for the Old 3DS
 * (docs/research/native/hwprof-progress.md). Two levers, both DEFAULT OFF, both diagnostic only
 * (nothing here reads or writes game state; zero heap; every array is static):
 *
 * [debug] c5pmu = 1 -- the CP15 performance counters per [c5cat] bucket. On every switch of the
 *     main-thread clock (gdx3ds_mprof_enter) the two event counters PMN0/PMN1 are read through
 *     svcControlPerformanceCounter and their deltas charged to the bucket being closed. One event
 *     PAIR per 64-iteration window, rotating through six pairs (c5pmu_pair = 0..5 pins one):
 *         0 inst,imiss   1 stall_if,stall_dh   2 drmiss,dwmiss   3 ext,stall_lsu
 *         4 tlbmiss,evict   5 brmiss,sbdrain
 *     One receipt per window, next to [c5cat] (same frame=, same normalisation: tick columns per
 *     tick, render columns per rendered frame, in thousands of events):
 *       [c5pmu] frame=N n= r= pair=K ev=a,b cal=x,y rd/t= | logic=a,b dlb=a,b oth=a,b | fbeg=a,b
 *               misc=a,b sky=a,b trk=a,b dress=a,b mach=a,b mesh=a,b shd=a,b fx=a,b hud=a,b
 *               mir=a,b swp=a,b fend=a,b
 *     `cal` = events one counter read pair itself adds (measured at every pair switch) and is
 *     already subtracted; rd/t = bucket switches per tick. The counters are per CORE and count
 *     every mode: kernel work on main's behalf (svc, IRQ) and any core-0 thread that preempts
 *     main while a bucket is open land in that bucket, exactly as they do in the [c5cat] clock.
 *     c5pmu = 2 is a DRY RUN (fake counters from the system tick) that exercises the whole path
 *     in the emulator, whose kernel does not implement the svc. It arms the [c5cat] clock by
 *     itself (no gputrace needed). Never touched: CCNT, SET_VALUE, RESET (the kernel's own tick).
 *     Agg: tools/native/c5pmu_agg.py (CPI, misses per kilo-instruction, stall shares).
 *
 * [debug] hwsite = 1 -- per-call micro-timers at the hardware suspects, attributed to the
 *     [c5cat] bucket that is open when the call is made. Sites (nested sites are counted in both;
 *     texup is inside import, heapflush/flush inside kick and fend):
 *         draw       citro3d draw submission (C3D_DrawArrays/Elements: context + uniform upload)
 *         texup      the backend's texture upload (decode->swizzle->linear copy->flush)
 *         import     the interpreter's ImportTexture (key hash, lookup, decode, upload)
 *         flush      GSPGPU_FlushDataCache under 1 MiB (VBO tail, textures, framebuffers)
 *         heapflush  GSPGPU_FlushDataCache of 1 MiB or more (the whole linear heap: C3D_FrameEnd,
 *                    overlap kicks)
 *         kick       an [overlap] mid-frame GX queue kick (heap flush + split + queue run)
 *         rplrec     C6 replay recording (draw scan + G_MTX/POPMTX record), the tax of replay=1
 *         hudret     hud_retain's run begin (fingerprint + replay on a hit, record setup on a miss)
 *         hudrec     hud_retain's run end (recording a run)
 *     One receipt per site per [c5cat] window:
 *       [hwsite] frame=N r=R site=S n/f=calls us/f=us ovh/f=us | cat=n/us cat=n/us ...
 *     per rendered frame; ovh = the site's own two clock reads per call.
 *     [debug] hwsite_every = N (onesubmit, default 1): time the sites in one [c5cat] window of
 *     every N only (receipts for those windows only), the cheap sampled mode for a test card.
 */
#ifndef GDX3DS_HWPROF_H
#define GDX3DS_HWPROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GDX3DS_HWS_DRAW = 0,
    GDX3DS_HWS_TEXUP,
    GDX3DS_HWS_IMPORT,
    GDX3DS_HWS_FLUSH,
    GDX3DS_HWS_HEAPFLUSH,
    GDX3DS_HWS_KICK,
    GDX3DS_HWS_RPLREC,
    GDX3DS_HWS_HUDRET,
    GDX3DS_HWS_HUDREC,
    GDX3DS_HWS_COUNT
};

/* Weak: the gfx library and the vendored citro3d are linked into host tests and harnesses that
 * do not carry this TU; there every site is one address test. In the game, gdx3ds_hws_on is the
 * latched lever. */
extern int gdx3ds_hws_on __attribute__((weak));
uint64_t gdx3ds_hws_now(void) __attribute__((weak));
void gdx3ds_hws_add(int site, uint64_t t0) __attribute__((weak));
/* GSPGPU_FlushDataCache, timed as flush / heapflush when the lever is on. */
void gdx3ds_hws_gsp_flush(const void* addr, uint32_t size) __attribute__((weak));

/* hwprof2: every data-cache flush the port makes, by the reason it is made. With [debug] hwsite=1
 * one [hwflush] line per [c5cat] window counts and times them per kind (per rendered frame); the
 * plain gdx3ds_hws_gsp_flush above is HEAPEND for >= 1 MiB (C3D_FrameEnd(0)) and OTHER below.
 * [debug] flush_svc=1 (DEFAULT 0) issues the ranges under 1 MiB as svcFlushProcessDataCache on this
 * process instead of the GSPGPU_FlushDataCache IPC: the same clean+invalidate of the same range
 * without the round trip to the GSP sysmodule (hardware, r60probe F: 32 B 54.9 us via GSP vs 7.8 us
 * by svc). Whole-heap flushes stay on GSP. */
enum {
    GDX3DS_HWFL_VBO = 0,  /* the VBO tail before a submission (FrameEnd, kicks, splits) */
    GDX3DS_HWFL_TEX,      /* a texture upload / atlas page write */
    GDX3DS_HWFL_RES,      /* resident track / mesh data written at bake */
    GDX3DS_HWFL_SWAP,     /* a screen framebuffer before a present (swp_lean) */
    GDX3DS_HWFL_HEAPEND,  /* the whole linear heap in C3D_FrameEnd(0) */
    GDX3DS_HWFL_HEAPKICK, /* the whole linear heap before an [overlap] kick */
    GDX3DS_HWFL_OTHER,
    GDX3DS_HWFL_COUNT
};
void gdx3ds_hws_gsp_flush_kind(const void* addr, uint32_t size, int kind) __attribute__((weak));

/* 0 when the lever is off (or this TU's caller is linked without the module). */
static inline uint64_t gdx3ds_hws_begin(void) {
    if (&gdx3ds_hws_on == 0 || gdx3ds_hws_on == 0) {
        return 0;
    }
    return gdx3ds_hws_now();
}

static inline void gdx3ds_hws_end(int site, uint64_t t0) {
    if (t0 != 0) {
        gdx3ds_hws_add(site, t0);
    }
}

/* mainprof's hooks (gdx3ds_mainprof.c, main thread, between frames). latch reads both levers
 * once the config is in and returns 1 when either wants the [c5cat] clock armed; rebase starts
 * a frame's counter baseline (the gap between frames is charged to nobody); emit writes the
 * window's receipts and rotates the event pair. */
int gdx3ds_hwprof_latch(void);
void gdx3ds_c5pmu_rebase(void);
void gdx3ds_hwprof_emit(unsigned long frame, unsigned iterations, unsigned rendered);

#ifdef __cplusplus
}

/* RAII form for C++ sites with several exits. */
struct GdxHwsScope {
    int site;
    uint64_t t0;
    explicit GdxHwsScope(int s) : site(s), t0(gdx3ds_hws_begin()) {}
    ~GdxHwsScope() { gdx3ds_hws_end(site, t0); }
    GdxHwsScope(const GdxHwsScope&) = delete;
    GdxHwsScope& operator=(const GdxHwsScope&) = delete;
};
#endif

#endif /* GDX3DS_HWPROF_H */
