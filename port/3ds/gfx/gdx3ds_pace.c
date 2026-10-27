/* port/3ds/gfx/gdx3ds_pace.c — n60gp: the swap-anchored vblank gate. Contract in the header. */
#include "gdx3ds_pace.h"

#include <stdarg.h>
#include <stdio.h>

#include <3ds.h>
#include <citro3d.h>

#include "../vendor/citro3d/gdx/c3d_gdx.h"

/* Weak like gdx3ds_gpu_prof.c: the DL-test harness links the gfx library without gdx3ds_os, and a
 * build with the vendored citro3d off has no swap stamp (the gate then refuses). */
extern int gdx3ds_config_get_int(const char* section, const char* key, int fallback) __attribute__((weak));
extern int gdx3ds_config_loaded(void) __attribute__((weak));
extern void gdx3ds_filelog_write(const char* msg, size_t len) __attribute__((weak));
extern void C3Dgdx_SwapStampRead(unsigned out[2]) __attribute__((weak));
extern unsigned long long C3Dgdx_SwapStampTick(void) __attribute__((weak));
/* n60stereo: 1 while the 3D right-eye pass runs this frame (gdx3ds_stereo_frame.cpp). */
extern int gdx3ds_stereo_active(void) __attribute__((weak));
/* o3ds60 [debug] top_triple (port/3ds/gdx3ds_pipe.h; vendor/libctru_gfx/gfx_split.c, weak: the DL
 * harness links neither): while the third top framebuffer rotates, the end guard has nothing to
 * protect (the back buffer is never on screen nor pending) and does not wait. */
extern bool gdx3ds_gfx_top_triple_active(void) __attribute__((weak));

enum { kPaceReceiptBegins = 256 };

enum PaceMode {
    kPaceStock = 0,  /* the CADENCE rule (gdx3ds_gpu_prof.c) */
    kPaceHybrid = 1, /* begin: no vblank wait once one fired since the previous swap; end guard */
    kPaceEnd = 2,    /* begin: never a vblank wait (fresh stamp); end guard only */
};

typedef struct PaceWindow {
    uint32_t begins;
    uint32_t waits;
    uint32_t skips;
    uint32_t longWaits;
    uint32_t traps;
    uint32_t nostamps;
    uint32_t endWaits;
    uint64_t waitTicks;
    uint64_t endWaitTicks;
    uint64_t workTicks;
    uint32_t swapsAtStart;
    uint32_t vblAtStart;
    uint32_t vbl1AtStart;
    uint32_t hold[4];       /* n60stereo: presented frames held 1 / 2 / 3 / 4+ vblanks */
    uint32_t tails;         /* frames whose GPU tail (C3D_FrameEnd -> top swap request) was measured */
    uint64_t tailTicks;
    uint64_t tailMax;
    uint32_t tailLate;      /* tails longer than 4 ms */
    uint32_t tbSkips;       /* o3ds60: end guards that did not wait because the top triple buffer ran */
} PaceWindow;

static int sLatched = 0;
static int sArmed = 0;
static int sMode = kPaceStock;
static int sMode3d = -1;           /* n60stereo [debug] pace_swapgate_3d: the rule while 3D runs (-1 = sMode) */
static uint64_t sFrameEndTick = 0; /* the latest end guard (right before C3D_FrameEnd) */
static uint32_t sHoldPrevSwap = 0; /* the swap count / vblank of the previous presented frame */
static uint32_t sHoldPrevVbl = 0;
static int sHoldPrevKnown = 0;
static uint32_t sAbPeriod = 0; /* 0 = no A/B */
static int sReceipt = 0;
static uint32_t sSpinUs = 0;
static uint32_t sSpinJitterUs = 0;
static uint32_t sSpinState = 0x9e3779b9u;

static uint32_t sFreshBegins = 0;
static uint32_t sPrevSwapCount = 0;
static uint64_t sPrevBeginReturn = 0;
static int sFramePhase = kPaceStock; /* the rule of the frame being built (its end guard follows it) */
static int sWindowPhase = -1;
static int sWindowTb = -1; /* o3ds60: the top triple buffer's state when the window opened */
static PaceWindow sWin;
static GdxPaceFrameInfo sInfo;          /* n60st3: the latest fresh begin, for [debug] spike_ms */
static uint64_t sEndGuardWaitTicks = 0; /* the latest end guard's wait */

static void PaceLog(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    const size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    svcOutputDebugString(buf, len);
    if (&gdx3ds_filelog_write != NULL) {
        gdx3ds_filelog_write(buf, len);
    }
}

static int ConfigInt(const char* key, int fallback) {
    return gdx3ds_config_get_int("debug", key, fallback);
}

static int ConfigReady(void) {
    if (&gdx3ds_config_get_int == NULL) {
        return 0;
    }
    return &gdx3ds_config_loaded == NULL || gdx3ds_config_loaded();
}

static uint32_t RoundAbPeriod(int period) {
    if (period <= 0) {
        return 0;
    }
    uint32_t p = period >= kPaceReceiptBegins ? (uint32_t)period : (uint32_t)kPaceReceiptBegins;
    return p - p % kPaceReceiptBegins; /* a receipt window never straddles a phase */
}

static void LatchConfig(void) {
    sLatched = 1;
    sMode = ConfigInt("pace_swapgate", 0);
    if (sMode < kPaceStock || sMode > kPaceEnd) {
        sMode = kPaceStock;
    }
    sAbPeriod = RoundAbPeriod(ConfigInt("pace_ab_period", 0));
    sMode3d = ConfigInt("pace_swapgate_3d", -1);
    if (sMode3d > kPaceEnd || &gdx3ds_stereo_active == NULL) {
        sMode3d = -1;
    }
    sReceipt = ConfigInt("pace_receipt", 0) != 0;
    const int spin = ConfigInt("pace_spin_us", 0);
    const int jitter = ConfigInt("pace_spin_jitter_us", 0);
    sSpinUs = spin > 0 ? (uint32_t)spin : 0u;
    sSpinJitterUs = jitter > 0 ? (uint32_t)jitter : 0u;
    if ((sMode != kPaceStock || sMode3d > kPaceStock) && &C3Dgdx_SwapStampRead == NULL) {
        PaceLog("[pace] swapgate=%d swapgate_3d=%d REFUSED: the vendored citro3d swap stamp is not linked", sMode,
                sMode3d);
        sMode = kPaceStock;
        sMode3d = -1;
    }
    sArmed = sMode != kPaceStock || sMode3d > kPaceStock || sReceipt || sSpinUs != 0 || sSpinJitterUs != 0;
    if (sArmed) {
        PaceLog("[pace] init swapgate=%d swapgate_3d=%d ab_period=%lu receipt=%d spin_us=%lu jitter_us=%lu", sMode,
                sMode3d, (unsigned long)sAbPeriod, sReceipt, (unsigned long)sSpinUs, (unsigned long)sSpinJitterUs);
    }
}

int gdx3ds_pace_armed(void) {
    if (!sLatched) {
        if (!ConfigReady()) {
            return 0;
        }
        LatchConfig();
    }
    return sArmed;
}

static uint64_t UsToTicks(uint32_t us) {
    return (uint64_t)us * (uint64_t)(SYSCLOCK_ARM11 / 1000000u);
}

static float TicksToMs(uint64_t ticks) {
    return (float)ticks / (float)(SYSCLOCK_ARM11 / 1000u);
}

static uint32_t NextSpinRandom(void) {
    sSpinState ^= sSpinState << 13;
    sSpinState ^= sSpinState >> 17;
    sSpinState ^= sSpinState << 5;
    return sSpinState;
}

static void SpinSyntheticWork(void) {
    uint32_t us = sSpinUs;
    if (sSpinJitterUs != 0) {
        us += NextSpinRandom() % sSpinJitterUs;
    }
    if (us == 0) {
        return;
    }
    const uint64_t until = svcGetSystemTick() + UsToTicks(us);
    while (svcGetSystemTick() < until) {
        __asm__ volatile("" ::: "memory");
    }
}

static void ReadSwapStamp(unsigned stamp[2]) {
    if (&C3Dgdx_SwapStampRead != NULL) {
        C3Dgdx_SwapStampRead(stamp);
    } else {
        stamp[0] = 0;
        stamp[1] = 0;
    }
}

static int InEvenAbBlock(void) {
    return sAbPeriod != 0 && ((sFreshBegins / sAbPeriod) & 1u) == 0;
}

/* The rule of this begin: the configured mode, or with an A/B period stock in even blocks. While
 * the 3D right eye runs, [debug] pace_swapgate_3d replaces the rule, and an A/B period alternates
 * pace_swapgate (even blocks) with pace_swapgate_3d (odd blocks): the 3D card A/Bs the two gates. */
static int ActivePhase(void) {
    if (sMode3d >= 0 && gdx3ds_stereo_active()) {
        return InEvenAbBlock() ? sMode : sMode3d;
    }
    if (InEvenAbBlock()) {
        return kPaceStock;
    }
    return sMode;
}

/* Wait until VBlank0 has fired at least once after the counter value `since` (citro3d's own
 * C3D_FrameSync loop, on the top screen's counter only: the top swap is what the gate tracks). */
static void WaitVblank0After(uint32_t since) {
    while (C3D_FrameCounter(0) == since) {
        gspWaitForAnyEvent();
    }
}

static int TopTripleActive(void) {
    return (&gdx3ds_gfx_top_triple_active != NULL && gdx3ds_gfx_top_triple_active()) ? 1 : 0;
}

static void OpenWindow(int phase) {
    unsigned stamp[2];
    ReadSwapStamp(stamp);
    sWin = (PaceWindow){0};
    sWin.swapsAtStart = stamp[0];
    sWin.vblAtStart = C3D_FrameCounter(0);
    sWin.vbl1AtStart = C3D_FrameCounter(1);
    sWindowPhase = phase;
    sWindowTb = TopTripleActive();
}

static void EmitWindow(void) {
    unsigned stamp[2];
    ReadSwapStamp(stamp);
    const uint32_t swaps = stamp[0] - sWin.swapsAtStart;
    const uint32_t vbl = C3D_FrameCounter(0) - sWin.vblAtStart;
    const uint32_t vbl1 = C3D_FrameCounter(1) - sWin.vbl1AtStart;
    const float inv = sWin.begins != 0 ? 1.0f / (float)sWin.begins : 0.0f;
    const float tinv = sWin.tails != 0 ? 1.0f / (float)sWin.tails : 0.0f;
    PaceLog("[pace] frame=%lu n=%lu phase=%d | swaps=%lu vbl=%lu rep=%ld vbl1=%lu | waits=%lu skips=%lu "
            "long=%lu trap=%lu nostamp=%lu eg=%lu | wait_ms=%.2f eg_ms=%.2f work_ms=%.2f | hold=%lu/%lu/%lu/%lu "
            "tail_ms=%.2f max=%.2f late=%lu tb=%d tbskip=%lu",
            (unsigned long)sFreshBegins, (unsigned long)sWin.begins, sWindowPhase, (unsigned long)swaps,
            (unsigned long)vbl, (long)vbl - (long)swaps, (unsigned long)vbl1, (unsigned long)sWin.waits,
            (unsigned long)sWin.skips, (unsigned long)sWin.longWaits, (unsigned long)sWin.traps,
            (unsigned long)sWin.nostamps, (unsigned long)sWin.endWaits, (double)(TicksToMs(sWin.waitTicks) * inv),
            (double)(TicksToMs(sWin.endWaitTicks) * inv), (double)(TicksToMs(sWin.workTicks) * inv),
            (unsigned long)sWin.hold[0], (unsigned long)sWin.hold[1], (unsigned long)sWin.hold[2],
            (unsigned long)sWin.hold[3], (double)(TicksToMs(sWin.tailTicks) * tinv), (double)TicksToMs(sWin.tailMax),
            (unsigned long)sWin.tailLate, sWindowTb, (unsigned long)sWin.tbSkips);
}

/* n60stereo: at a fresh begin (the previous frame's queue is done, its swap requested): how many
 * vblanks the frame before it was held on screen (the vblank counter between consecutive top swap
 * requests: each is latched at the next VBlank0), and the previous frame's GPU tail (its
 * C3D_FrameEnd to its swap request). Only consecutive swaps count; a gap resets the chain. */
static void NoteSwapTiming(void) {
    unsigned stamp[2];
    ReadSwapStamp(stamp);
    sInfo.swapGap = 0;
    sInfo.prevGpuTailTicks = 0;
    if (sHoldPrevKnown && stamp[0] == sHoldPrevSwap + 1u) {
        const uint32_t held = stamp[1] - sHoldPrevVbl;
        sWin.hold[held >= 4u ? 3 : (held == 0u ? 0 : held - 1u)]++;
        sInfo.swapGap = held;
        if (&C3Dgdx_SwapStampTick != NULL && sFrameEndTick != 0) {
            const uint64_t swapTick = C3Dgdx_SwapStampTick();
            if (swapTick > sFrameEndTick) {
                const uint64_t tail = swapTick - sFrameEndTick;
                sInfo.prevGpuTailTicks = tail;
                sWin.tails++;
                sWin.tailTicks += tail;
                sWin.tailMax = tail > sWin.tailMax ? tail : sWin.tailMax;
                sWin.tailLate += tail > UsToTicks(4000) ? 1u : 0u;
            }
        }
    }
    sHoldPrevKnown = stamp[0] != sHoldPrevSwap || !sHoldPrevKnown;
    sHoldPrevSwap = stamp[0];
    sHoldPrevVbl = stamp[1];
}

/* n60st3: the previous frame's render span (begin return -> end guard entry) and end-guard wait. */
static void NotePreviousFrame(void) {
    const int known = sPrevBeginReturn != 0 && sFrameEndTick > sPrevBeginReturn;
    sInfo.prevRenderTicks = known ? sFrameEndTick - sPrevBeginReturn : 0;
    sInfo.prevEndGuardTicks = known ? sEndGuardWaitTicks : 0;
}

/* A VBlank0 already fired after the previous frame's swap request, yet the stock rule (less than a
 * period since the previous begin returned) waits for the next one: the phase trap. */
static int IsTrap(int stockSkip) {
    unsigned stamp[2];
    ReadSwapStamp(stamp);
    const int freshStamp = stamp[0] != sPrevSwapCount;
    return !stockSkip && freshStamp && C3D_FrameCounter(0) != stamp[1];
}

/* Stock: the vblank wait first (unless the CADENCE rule skips it), then the GX queue wait --
 * exactly C3D_FrameBegin(stockSkip ? 0 : C3D_FRAME_SYNCDRAW). Returns 1 when it waited. */
static int BeginStock(int stockSkip, uint64_t* queueTicks, uint64_t* vblankTicks) {
    const uint64_t t0 = svcGetSystemTick();
    if (!stockSkip) {
        C3D_FrameSync();
    }
    const uint64_t t1 = svcGetSystemTick();
    C3D_FrameBegin(0);
    *vblankTicks = t1 - t0;
    *queueTicks = svcGetSystemTick() - t1;
    return !stockSkip;
}

/* Swap gate: the GX queue wait first (after it, the previous frame's top swap request is
 * stamped), then the begin's vblank wait by the phase's rule. Without a fresh stamp: the stock
 * rule. Returns 1 when it waited. */
static int BeginGate(int phase, int stockSkip, uint64_t* queueTicks, uint64_t* vblankTicks) {
    const uint64_t t0 = svcGetSystemTick();
    C3D_FrameBegin(0);
    const uint64_t t1 = svcGetSystemTick();
    unsigned stamp[2];
    ReadSwapStamp(stamp);
    int wait = 0;
    if (stamp[0] == sPrevSwapCount) {
        sWin.nostamps++;
        wait = !stockSkip;
        if (wait) {
            C3D_FrameSync();
        }
    } else if (phase == kPaceHybrid) {
        wait = !stockSkip && C3D_FrameCounter(0) == stamp[1];
        if (wait) {
            WaitVblank0After(stamp[1]);
        }
    }
    *queueTicks = t1 - t0;
    *vblankTicks = svcGetSystemTick() - t1;
    return wait;
}

static void CountBegin(int waited, uint64_t vblankTicks) {
    if (waited) {
        sWin.waits++;
    } else {
        sWin.skips++;
    }
    sWin.waitTicks += vblankTicks;
    if (vblankTicks > UsToTicks(16700)) {
        sWin.longWaits++; /* a vblank wait longer than one LCD period */
    }
    sWin.begins++;
}

void gdx3ds_pace_frame_begin(int (*stockSkip)(void), uint64_t* queueTicks, uint64_t* vblankTicks) {
    SpinSyntheticWork();
    const int phase = ActivePhase();
    if (phase != sWindowPhase || TopTripleActive() != sWindowTb) {
        if (sReceipt && sWin.begins >= kPaceReceiptBegins / 4u) {
            EmitWindow(); /* o3ds60: a window never straddles a triple-buffer edge */
        }
        OpenWindow(phase);
    }
    const uint64_t tCall = svcGetSystemTick();
    if (sPrevBeginReturn != 0) {
        sWin.workTicks += tCall - sPrevBeginReturn; /* the iteration's work, synthetic spin included */
    }
    NotePreviousFrame();
    const int skip = stockSkip();
    if (IsTrap(skip)) {
        sWin.traps++;
    }
    uint64_t q = 0;
    uint64_t v = 0;
    const int waited = phase == kPaceStock ? BeginStock(skip, &q, &v) : BeginGate(phase, skip, &q, &v);
    NoteSwapTiming();
    unsigned stamp[2];
    ReadSwapStamp(stamp);
    sPrevSwapCount = stamp[0];
    sPrevBeginReturn = svcGetSystemTick();
    sFramePhase = phase;
    CountBegin(waited, v);
    sFreshBegins++;
    sInfo.queueTicks = q;
    sInfo.vblankTicks = v;
    sInfo.phase = phase;
    if (queueTicks != NULL) {
        *queueTicks = q;
    }
    if (vblankTicks != NULL) {
        *vblankTicks = v;
    }
    if (sReceipt && sWin.begins >= kPaceReceiptBegins) {
        EmitWindow();
        OpenWindow(ActivePhase());
    }
}

void gdx3ds_pace_frame_end_guard(void) {
    sFrameEndTick = svcGetSystemTick();
    sEndGuardWaitTicks = 0;
    if (sFramePhase == kPaceStock) {
        return;
    }
    unsigned stamp[2];
    ReadSwapStamp(stamp);
    if (C3D_FrameCounter(0) != stamp[1]) {
        return; /* the previous frame's swap request has been latched (or there was none) */
    }
    if (TopTripleActive()) {
        sWin.tbSkips++; /* o3ds60: this frame's transfer targets the free third buffer */
        return;
    }
    const uint64_t t0 = svcGetSystemTick();
    WaitVblank0After(stamp[1]);
    sEndGuardWaitTicks = svcGetSystemTick() - t0;
    sWin.endWaits++;
    sWin.endWaitTicks += sEndGuardWaitTicks;
}

void gdx3ds_pace_frame_info(GdxPaceFrameInfo* out) {
    *out = sInfo;
}
