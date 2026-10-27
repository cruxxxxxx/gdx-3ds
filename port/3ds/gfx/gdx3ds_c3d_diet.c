/* port/3ds/gfx/gdx3ds_c3d_diet.c -- c5submit: [native] c3d_diet levers + receipts
 * (contract in gdx3ds_c3d_diet.h). */
#include "gdx3ds_c3d_diet.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <3ds.h>

#if defined(GDX3DS_VENDOR_CITRO3D)
#include "c3d_gdx.h"
#endif

extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));

#define C3DDIET_WINDOW 64u

static int sLatched = 0;

static void DietLog(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    if (&gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(buf, len);
    }
}

static int ConfigInt(const char* section, const char* key, int fallback) {
    if (&gdx3ds_config_get_int == NULL) {
        return fallback;
    }
    return gdx3ds_config_get_int(section, key, fallback);
}

static int ConfigReady(void) {
    return &gdx3ds_config_loaded == NULL || gdx3ds_config_loaded();
}

#if defined(GDX3DS_VENDOR_CITRO3D)

static unsigned long sFrames = 0;
static unsigned sWinFrames = 0;
static uint32_t sReadTicks = 0;
static int sProf = 0;
/* onesubmit [debug] c3dprof_every = N (default 1): the profile runs in one 64-frame window of every
 * N (the mode word's profile bits are cleared in the others), the cheap sampled mode for a card. */
static unsigned sProfEvery = 1;
static unsigned sProfWindow = 0;
static uint32_t sProfBits = 0;
static int sVerify = 0;
/* Verify totals since arming (the window lines carry the window's own counts too). */
static unsigned long sVfyTotal = 0;
static unsigned long sVfyBadWords = 0;
static unsigned long sVfyBadState = 0;
static unsigned long sVfyUnverifiable = 0;

static uint32_t ModeFromConfig(int diet, int mask, int verify, int prof) {
    uint32_t mode = diet ? ((uint32_t)mask & C3DGDX_DIET_MASK) : 0u;
    if (verify) {
        mode |= C3DGDX_VERIFY;
    }
    if (verify == 2) {
        mode |= C3DGDX_VERIFY_SELFTEST;
    }
    if (prof == 1) {
        mode |= C3DGDX_PROF_SPLIT;
    } else if (prof == 2) {
        mode |= C3DGDX_PROF_TOTAL;
    }
    return mode;
}

static void LatchConfig(void) {
    if (!ConfigReady()) {
        return; /* not parsed yet: re-check at the next fresh frame */
    }
    sLatched = 1;
    const int diet = ConfigInt("native", "c3d_diet", 0) != 0;
    const int mask = ConfigInt("native", "c3d_diet_mask", (int)C3DGDX_DIET_ALL);
    sVerify = ConfigInt("native", "c3d_diet_verify", 0);
    sProf = ConfigInt("debug", "c3dprof", 0);
    C3Dgdx_mode = ModeFromConfig(diet, mask, sVerify, sProf);
    sProfBits = C3Dgdx_mode & (C3DGDX_PROF_SPLIT | C3DGDX_PROF_TOTAL);
    const int every = ConfigInt("debug", "c3dprof_every", 1);
    sProfEvery = every > 1 ? (unsigned)every : 1u;
    if (sProf != 0) {
        sReadTicks = C3Dgdx_Calibrate();
    }
    DietLog("[c3ddiet] init diet=%d mask=0x%x verify=%d prof=%d vendored=1 mode=0x%lx read_ticks=%lu (%s)", diet,
            (unsigned)(mask & C3DGDX_DIET_MASK), sVerify, sProf, (unsigned long)C3Dgdx_mode, (unsigned long)sReadTicks,
            C3Dgdx_mode == 0 ? "stock" : "on");
}

static float TicksToUs(double ticks) {
    return (float)(ticks * 1000.0 / (double)CPU_TICKS_PER_MSEC);
}

static float PerDraw(uint64_t v, uint32_t draws) {
    return draws ? (float)((double)v / (double)draws) : 0.0f;
}

/* Section time per draw in µs, one clock read taken out (each section interval holds one). */
static float SectionUs(const C3Dgdx_Stats* s, int sec) {
    if (s->splitDraws == 0) {
        return 0.0f;
    }
    double ticks = (double)s->secTicks[sec] / (double)s->splitDraws;
    /* DRAW is closed twice per draw (its prologue, then its packets). */
    const double reads = sec == C3DGDX_SEC_DRAW ? 2.0 : 1.0;
    ticks -= reads * (double)sReadTicks;
    return TicksToUs(ticks);
}

static void EmitSplit(const C3Dgdx_Stats* s) {
    static const char* const kNames[C3DGDX_SEC_COUNT] = { "fb",  "prog", "attr", "buf", "eff", "tex",
                                                          "teb", "tev",  "lf",   "vsu", "gsu", "drw" };
    char part[C3DGDX_SEC_COUNT][40];
    float sum = 0.0f;
    int sec;
    for (sec = 0; sec < C3DGDX_SEC_COUNT; sec++) {
        const float us = SectionUs(s, sec);
        sum += us;
        snprintf(part[sec], sizeof(part[sec]), "%s=%.2f/%.0f%%/%.1f", kNames[sec], us,
                 s->splitDraws ? 100.0f * (float)s->secFired[sec] / (float)s->splitDraws : 0.0f,
                 PerDraw(s->secWords[sec], s->splitDraws));
    }
    DietLog("[c3dprof] frame=%lu n=%u draws=%lu sum_us=%.2f | %s %s %s %s %s %s", sFrames, sWinFrames,
            (unsigned long)s->splitDraws, sum, part[0], part[1], part[2], part[3], part[4], part[5]);
    DietLog("[c3dprof] frame=%lu | %s %s %s %s %s %s | us/%%fired/words per draw, read_us=%.2f", sFrames, part[6],
            part[7], part[8], part[9], part[10], part[11], TicksToUs((double)sReadTicks));
}

static void EmitTotal(const C3Dgdx_Stats* s) {
    const double ticks = s->totDraws ? (double)s->totTicks / (double)s->totDraws : 0.0;
    DietLog("[c3dprof] frame=%lu n=%u total draws=%lu us/draw=%.2f (read pair out: %.2f) mode=0x%lx", sFrames,
            sWinFrames, (unsigned long)s->totDraws, TicksToUs(ticks - (double)sReadTicks), TicksToUs(ticks),
            (unsigned long)C3Dgdx_mode);
}

static void EmitWindow(void) {
    C3Dgdx_Stats s;
    C3Dgdx_GetStats(&s, 1);
    const float inv = 1.0f / (float)sWinFrames;
    sVfyTotal += s.verified;
    sVfyBadWords += s.mismatchWords;
    sVfyBadState += s.mismatchState;
    sVfyUnverifiable += s.unverifiable;
    DietLog("[c3ddiet] frame=%lu n=%u draws/f=%.1f words/draw=%.1f unif=%.2f/draw same=%.0f%% elided=%.2f/draw vfy=%lu bad=%lu/%lu "
            "unv=%lu | total vfy=%lu bad=%lu/%lu unv=%lu first=%lu@%lu stock=%08lx diet=%08lx",
            sFrames, sWinFrames, (float)s.draws * inv, PerDraw(s.words, s.draws), PerDraw(s.unifVec4, s.draws),
            s.unifVec4 ? 100.0f * (float)s.unifSame / (float)s.unifVec4 : 0.0f, PerDraw(s.unifElided, s.draws),
            (unsigned long)s.verified,
            (unsigned long)s.mismatchWords, (unsigned long)s.mismatchState, (unsigned long)s.unverifiable, sVfyTotal,
            sVfyBadWords, sVfyBadState, sVfyUnverifiable, (unsigned long)s.firstBadDraw,
            (unsigned long)s.firstBadWord, (unsigned long)s.firstBadStock, (unsigned long)s.firstBadDiet);
    const int profiled = (sProfWindow % sProfEvery) == 0u;
    if (sProf == 1 && profiled) {
        EmitSplit(&s);
    } else if (sProf == 2 && profiled) {
        EmitTotal(&s);
    }
    sProfWindow++;
    if ((sProfWindow % sProfEvery) == 0u) { /* the next window's sampling */
        C3Dgdx_mode |= sProfBits;
    } else {
        C3Dgdx_mode &= ~sProfBits;
    }
}

void gdx3ds_c3d_diet_frame_begin(void) {
    if (!sLatched) {
        LatchConfig();
    }
}

void gdx3ds_c3d_diet_frame_end(void) {
    if (!sLatched || (!sVerify && sProf == 0)) {
        return;
    }
    sFrames++;
    sWinFrames++;
    if (sWinFrames >= C3DDIET_WINDOW) {
        EmitWindow();
        sWinFrames = 0;
    }
}

#else /* prebuilt libcitro3d.a: no diet to arm */

void gdx3ds_c3d_diet_frame_begin(void) {
    if (sLatched || !ConfigReady()) {
        return;
    }
    sLatched = 1;
    const int diet = ConfigInt("native", "c3d_diet", 0) != 0;
    DietLog("[c3ddiet] init diet=%d vendored=0 (%s)", diet, diet ? "refused: prebuilt citro3d" : "stock");
}

void gdx3ds_c3d_diet_frame_end(void) {
}

#endif
